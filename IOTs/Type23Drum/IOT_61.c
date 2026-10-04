/*
 * This is an implementation of the PDP-1 Type 23 Parallel Drum.
 * The drum data is stored in '/opt/pidp1-mods/pdp23drum' and is a binary image of the drum,
 * stored as 18 bit pdp-1 words per 32 bit image word.
 * The drum also uses IOTs 62 and 63, which alias to this one.
 * While the emulator runs the drum is a copy of the file in memory: a transfer reads and writes
 * the copy, and each write is queued for the file on a writer thread (writeBehind.c), so a slow
 * card or disk never holds the emulator thread. After any halt, and after another program closes
 * the file it wrote or renames a file over it, the file is looked at again, so a change made to
 * it is read in before the next transfer, with the machine running or not.
 *
 * wje 20-Jun-2026 - cleanup, begin this revision history, wasn't initially included
 * wje 3-Jul-2026 - set TE error if a write fails
 * wje 6-Jul-2026 - completely rework drum timing, now based on real time, the drum was always spinning,
 *    switch to THREADED mode so real cycle-stealing happens,
 *    change dcl completion timing to account for the cycle-stealing the high speed channel does.
 * wje 14-Jul-2026 - cks drp setting fix, check for HSCexecute() returning busy, fix lost static on transferCount.
 * wje 2-Sep-2026 - switch to use new TRUESTEAL mode for accurate cycle stealing
 * wje 22-Sep-2026 - move drum timing from wall-clock (now()) to simtime, so the drum is
 *    unaffected by throttle pauses and stays consistent across a halt or power cycle
 * 23-Sep-2026 claude - poll every cycle while the channel is busy, which an HSC side effect
 *    did before.
 * 24-Sep-2026 claude - a dba break survives the dwc and dcl that follow it and fires when the drum
 *    reaches the dba address, not about 0.7 of the distance past it.
 * 24-Sep-2026 claude - a write that wraps past drum address 7777 writes its second part from the
 *    words after the wrap, not from the start of the buffer again.
 * 27-Sep-2026 claude - a halt, however it happens, no longer drops or corrupts a transfer: one
 *    halted before its first word waits for the drum to come around again, one halted after it
 *    ends with the transfer error and writes only the words that moved. Every transfer now ends
 *    with a sequence break on the drum's channel.
 * 27-Sep-2026 claude - the drum is held in memory and written back through a write-behind queue,
 *    so a slow write no longer holds the emulator thread (up to 0.8 s was measured on a Pi 4 with
 *    a busy SD card). A failed write is reported on stderr: the transfer has ended by then, so it
 *    can no longer set the transfer error.
 * 29-Sep-2026 claude - a change another program makes to the drum file is seen at the next
 *    transfer, with the machine running, as it was before the drum was held in memory, not only
 *    after a halt. inotify reports the other program's close or rename, which the drum's own
 *    writes never raise. A reload that changes the drum is reported on stderr.
 * 29-Sep-2026 claude - with a dba armed, dcl polls in time for the break as well as for the
 *    transfer. Now that each enablePolling() starts a fresh count, polling for the transfer
 *    alone made the break up to 7 us late in the DBA, DWC, DCL sequence.
 * 4-Oct-2026 claude - a power cycle resets the drum control, as a STOP does not: a transfer and an
 *    armed dba are dropped, and busy and the transfer error cleared. A write the power cut short
 *    keeps on the drum the words that moved before it.
 */

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <sys/inotify.h>
#include <sys/stat.h>

#include "highSpeedChannels.h"
#include "iotHandler.h"
#include "writeBehind.h"

//#define DOLOGGING
#include "iotLogger.h"
#define LOG_START 0
#define LOG_IOT 0
#define LOG_POLL 0
#define LOG_HSC 0
#define LOG_TIME 0
#define LOG_TOTALTIME 0
#define LOG_BREAK 0
#define LOG_READ 0
#define LOG_WRITE 0

// Flag for busy for the cks instruction.
// DRP set is busy, cleared by operation completion, dia, or dba,
// from the DEC-1-137M diagnostic test program
#define CKS_DRP 0000001

#define HSC_CHAN 1      // drum uses 1

#define DRUMFILE "/opt/pidp1-mods/pdp23drum"
#define DRUMADDRTOSEEK(field, offset) ((((field) * 4096) + (offset)) * (int)sizeof(Word))
#define DRUMFIELDS 32                       // dia and dwc carry a 5-bit field
#define DRUMWORDS (DRUMFIELDS * 4096)
#define WBMAXBYTES (4 * 1024 * 1024)        // queued writes: about 14 s of back-to-back full tracks on a Pi 4

// No 18-bit word has this value, so a write-buffer word still holding it was never taken from core.
#define UNMOVED 0xffffffffU

static int drumFd = -1;
static Word drumImage[DRUMWORDS];   // the drum, as the transfers see it
static bool imageLoaded;
static bool lookAtFile;             // the machine halted since the last transfer looked at the file
static bool fileEvent;              // another program closed the file it wrote, or renamed one over it
static int watchFd = -1;            // inotify on the drum file's directory, -1 if there is none
static bool watchTried;             // watchFile() has run, so a failure is reported once
static char drumName[NAME_MAX + 1]; // the drum file's name in the watched directory
static WbQueueP wbP;                // NULL if it could not be made: writes are done in place
static pthread_mutex_t stampLock = PTHREAD_MUTEX_INITIALIZER;
static struct stat drumStamp;       // the file as the load or our own last write left it
static bool writeFailReported;      // written only on the writer thread
static int drumReadField;
static int drumWriteField;
static int drumAddr;
static int transferCount;           // set by dwc, consumed by dcl
static int sbsChan = 5;
static uint64_t rotationDelayTime;  // absolute simtime-scale target time for the drum to be in position
static uint64_t breakTime;          // absolute simtime-scale time the drum reaches the dba address

#ifdef LOG_TOTALTIME                // accumulate timing data
static uint64_t totalWords;         // cumulative number of words transferred, read and write
static uint64_t totalTime;          // accumulated actual transfer time
static uint64_t totalRequests;      // accumulated number of read or write requests
static uint64_t rqstStartTime;      // used to compute the timing delta
#endif

static int memBank;
static int memAddr;
static Word readBuffer[4096];
static Word writeBuffer[4096];

static bool readMode;
static bool writeMode;
static bool ioBusy;
static bool requestPending;
static bool dbaPending;
static bool teError;
static bool started;                // iotStart() has run once
static bool haltedWhileArmed;       // the machine halted while a transfer or a dba break waited
static uint64_t haltEndTime;        // simtime of the last halted pass that saw it
static bool powerCleared;           // a power clear came; the next iotIOPoll() clears busy in cks

static HSCChannelP chanP;   // how we get data
static HSCRequest request;

static void readDrumToBuffer(Word *, int, int, int);
static void writeBufferToDrum(Word *, int, int, int);
static void checkImage(void);
static void loadImage(void);
static bool fileChanged(void);
static void watchFile(void);
static void readWatch(void);
static void reloadImage(void);
static void queueWrite(int drumField, int drumAddr, int count);
static void writeDone(void *argP, int fd, bool ok);
static int drumLoc(PDP1 *pdp1P);
static int drumLocAt(uint64_t time);
static void resumeAfterHalt(void);
static int movedWords(void);

int
iotHandler(PDP1 *pdp1P, int dev, int pulse, int completion)
{
int stat;
int chanFlags;
int wordCount;
int breakCycles;

    if( pulse )
    {
        return(1);                  // only on one edge
    }

    iotCondLog(LOG_IOT, "In iot 61 as %o\n", dev);

    if( drumFd < 0 )
    {
        iotCondLog(LOG_IOT, "In iot 61, no drumFd\n");
        return(0);                 // sorry, some error with the drum file
    }

    if( completion )                    // we don't want to be
    {
        completion = 0;
        IOCOMPLETE(pdp1P);
    }

    switch( dev )
    {
    case 061:            // dia, drum initial address, in the IO register, or dba, drum break address
        dbaPending = requestPending = ioBusy = false;   // just to be sure
        CKS(pdp1P) &= ~CKS_DRP;       // and not busy

        readMode = IO(pdp1P) & 0400000;
        writeMode = 0;
        drumAddr = IO(pdp1P) & 07777;
        drumReadField = (IO(pdp1P) >> 12) & 037;

        if( MB(pdp1P) & 02000 )
        {
            // dba, using the interrupt system, reqiest break.
            // The break happens when the drum location == the drumAddr.
            iotCondLog(LOG_IOT, "dba, break on %o\n", drumAddr);
            if( drumAddr < drumLoc(pdp1P) )  // have to wait for it to come around again on the guitar
            {
                wordCount = 4096 - drumLoc(pdp1P) + drumAddr;
            }
            else
            {
                // Target is at or ahead of the current head position, no wraparound needed,
                // the wait is simply the forward distance from here to there.
                wordCount = drumAddr - drumLoc(pdp1P);
            }

            requestPending = ioBusy = false;
            if( wordCount < 1 )
            {
                wordCount = 1;
            }

            // The break has its own target, apart from rotationDelayTime, so that the dwc and dcl
            // of the manual's DBA, DWC, DCL sequence (H-23 p64 / 5-3) can start a transfer
            // without disarming it.
            breakTime = pdp1P->simtime + ((uint64_t)wordCount * 8500ULL);
            dbaPending = true;
            wordCount = (int)(((float)wordCount * 8500.0) / 5000.0) - 1;    // approximate polling delay
            if( wordCount < 1 )
            {
                wordCount = 1;
            }
            enablePolling(wordCount);
        }

        iotCondLog(LOG_IOT, "dia done, read %o, rfield %o, daddr %o\n", readMode, drumReadField, drumAddr);
        break;

    case 062:            // dwc, drum word count or dra, drum request address
        if( MB(pdp1P) & 02000 )
        {
            // dra, return current drum 'counter' in the IO register, along with status
            IO(pdp1P) = drumLoc(pdp1P);
            if( teError )
            {
                // Manual says we set bits 0 and 2: a transfer a halt cut short, or one the
                // channel refused.
                IO(pdp1P) |= 0500000;
                teError = false;
            }
            iotCondLog(LOG_IOT, "dra drum count %o\n", drumLoc(pdp1P));
        }
        else
        {
            writeMode = IO(pdp1P) & 0400000;
            drumWriteField = (IO(pdp1P) >> 12) & 037;
            transferCount = IO(pdp1P) & 07777;
            if( !transferCount )
            {
                transferCount = 4096;       // 0 means entire track
            }

#ifdef LOG_TOTALTIME
            totalWords += transferCount;
#endif
            iotCondLog(LOG_IOT, "dwc done, write %d, wfield %d, count %o\n", writeMode, drumWriteField, transferCount);
        }
        break;

    case 063:            // dcl, drum core location
        // Subcommand 20 is nonstandard behavior.
        // It was added to allow changing the drun's interrupt channel when using sbs16
        // in case it conflicts with other usage.
        // In practice, different PDP-1 installations could have different assignments, hardware configured.
        // If this is executed, the normal dcl setup does not happen.
        // If the interrupt channel is changed, the  prior channel is returned in the IO register.
        if( MB(pdp1P) & 02000 )
        {
            // enable/disable sbs16
            pdp1P->sbs16 = IO(pdp1P) & 040;
            stat = sbsChan;

            // change interrupt channel?
            if( IO(pdp1P) & 020 )
            {
                // The old value is returned in IO.
                sbsChan = IO(pdp1P) & 017;
                IO(pdp1P) = stat;
            }

            iotCondLog(LOG_IOT, "dss called with setting %02o, prior chnannel was %020\n", IO(pdp1P) & 077, stat);
            break;
        }

        // An armed dba break is kept: a program using sequence breaks starts a transfer with
        // DBA, DWC, DCL (H-23 p64 / 5-3).
        requestPending = ioBusy = false;

        // The manual says mem bank is bits 2, 3, but this isn't correct.
        // The hardware description is.
        // It's adtually bits 2-5 to support up to 16 memory modules.
        memBank = (IO(pdp1P) >> 12) & 017;      // support large memory PDP-1's
        memAddr = IO(pdp1P) & 07777;

        iotCondLog(LOG_IOT, "dcl 63 memBank %o memAddr %o\n", memBank, memAddr);

        // Set up the request for iotPoll() to submit it when the drum is positioned correctly.
        // Both the drum address and the memory address can wrap around.
        if( !readMode && !writeMode )
        {
            return(0);          // do nothing. An error?
        }

#ifdef LOG_TOTALTIME
        rqstStartTime = pdp1P->simtime;
        totalRequests++;
#endif
        checkImage();

        // The transfer happens immediately, but hsc will do the proper cycle-stealing before done
        chanFlags = HSC_MODE_TRUESTEAL | HSC_MODE_UPDATEPANEL;

        if( readMode )
        {
            chanFlags |= HSC_MODE_TOMEM;
            readDrumToBuffer(readBuffer, drumReadField, drumAddr, transferCount);
            iotCondLog(LOG_IOT, "dcl 63 requesting read\n");
        }

        if( writeMode )
        {
            chanFlags |= HSC_MODE_FROMMEM;
            // Marked so a transfer a halt cuts short writes only the words the channel took.
            for( stat = 0; stat < transferCount; ++stat )
            {
                writeBuffer[stat] = UNMOVED;
            }
            iotCondLog(LOG_IOT, "dcl 63 requesting write\n");
        }

        wordCount = 1;    // figure out how many drum word times for the drum to be in position, we need a least 1

        // Transferring a full mem bank is special, it can start anywhere, no rotational delay
        if( transferCount != 4096 )
        {
            if( drumAddr < drumLoc(pdp1P) )  // have to wait for it to come around again on the guitar
            {
                wordCount += 4096 - drumLoc(pdp1P) + drumAddr;
            }
            else
            {
                // Target is at or ahead of the current head position, no wraparound needed,
                // the wait is simply the forward distance from here to there.
                wordCount += drumAddr - drumLoc(pdp1P);
            }
        }

        // we assume we can proceed, manual says program should check status before calling IOT_61.
        request.mode = chanFlags;
        request.count = transferCount;
        request.memBank = memBank;
        request.memAddr = memAddr;
        request.toBufferP = readBuffer;
        request.fromBufferP = writeBuffer;
        request.wordTime = 85;              // 85 100ns delays, 8.5 usec
        requestPending = true;

        CKS(pdp1P) |= CKS_DRP;                 // busy until iotPoll signals real completion
        // Each drum word takes 8.5us; wordCount here is the rotational latency.
        // This uses simtime for the completion target, so it stays exact through throttle
        // pauses and host stalls instead of drifting with wall-clock time.
        rotationDelayTime = pdp1P->simtime + ((uint64_t)wordCount * 8500ULL);
        iotCondLog(LOG_TIME,"Completion target in %lu nsecs\n", rotationDelayTime - pdp1P->simtime);
        wordCount = (wordCount * 8500) / 5000;  // get the approximate polling delay

        // A dba armed before this dcl comes due a word before the transfer, and each arm starts
        // a fresh count, so poll for whichever is first, a cycle early as dia does.
        if( dbaPending )
        {
            breakCycles = 1;
            if( breakTime > pdp1P->simtime )
            {
                breakCycles = (int)((breakTime - pdp1P->simtime) / 5000ULL) - 1;
            }
            if( breakCycles < 1 )
            {
                breakCycles = 1;
            }
            if( breakCycles < wordCount )
            {
                wordCount = breakCycles;
            }
        }
        enablePolling(wordCount);               // real delay handled in iotPoll()
        break;

    default:
        return(0);                // should never happen
    }

    return(1);
}

void
iotStart()
{
    iotCondLog(LOG_START, "IOT 61 started\n");
    if( !wbP )
    {
        wbP = wbCreate(WBMAXBYTES);
    }

    if( drumFd < 0 )
    {
        drumFd = open(DRUMFILE, O_RDWR + O_CREAT, 0666);
        iotCondLog(LOG_START, "IOT 61 drumFd = %d\n", drumFd);
    }

    // After the open, so the file exists for realpath().
    if( !watchTried )
    {
        watchFile();
    }

    if( chanP == 0 )
    {
        chanP = HSCallocateChannel(HSC_CHAN);
        iotCondLog(LOG_START, "IOT 61 channel allocation %s\n", (chanP)?"ok":"failed");
    }

    // A STOP followed by Start or Continue is a halt like any other: a transfer or a dba break
    // in progress carries on through it, and iotPoll() makes it wait or end in error.
    // The transfer state is set up only on the first start.
    if( !started )
    {
        ioBusy = requestPending = dbaPending = 0;
        started = true;

#ifdef LOG_TOTALTIME
        totalTime = 0;
        totalWords = 0;
        totalRequests = 0;
#endif
    }

    // The drum's position is computed directly from simtime (drumLoc()), which already
    // starts from the wall clock at power-on, so there is no separate anchor to set up here.
}

// The close waits in the queue behind the drum's pending writes.
// The next start opens the file again, and the next transfer looks at it again.
void
iotStop()
{
    if( drumFd >= 0 )
    {
        if( wbP )
        {
            wbClose(wbP, drumFd);
        }
        else
        {
            close(drumFd);
        }

        drumFd = -1;
    }

    lookAtFile = true;

    iotCondLog(LOG_START, "IOT 61 stopped\n");
#ifdef LOG_TOTALTIME
    if( totalWords )
    {
        iotCondLog(LOG_TOTALTIME,"%lu words transferred in %lu usecs, %.2f usec/word, %.2f usec/request\n",
            totalWords,
            totalTime / 1000,
            (float)totalTime / (float)totalWords / 1000.0,
            (float)totalTime / (float)totalRequests / 1000.0);
        iotCondLog(LOG_TOTALTIME,"%d dcl requests\n", totalRequests);   // iotStop() has no PDP1*, so drumLoc() is unavailable here
    }

    // clear, iotStart() also does this
    totalTime = 0;
    totalWords = 0;
    totalRequests = 0;
#endif
    iotCloseLog();
}

// The power switch went off, the power clear resets the drum control.
// A transfer waiting for the/ drum or under way, and an armed dba, are dropped and the transfer error is cleared.
// A write cut short has already put the words that moved on the drum, so they are written, as at a halt's resume.
// Busy is in cks, which needs the PDP1, so the next iotIOPoll() clears it, the first pass with the power back on,
// before any Start.
// This comes before iotStop(), but a STOP before the power went off has already closed the drum file,
// so it is opened again for the write.
void
iotPowerClear(void)
{
int count;
bool reopened;

    count = 0;
    if( ioBusy && (request.mode & HSC_MODE_FROMMEM) )
    {
        count = movedWords();
    }

    if( count > 0 )
    {
        reopened = false;
        if( drumFd < 0 )
        {
            drumFd = open(DRUMFILE, O_RDWR + O_CREAT, 0666);
            reopened = true;
        }

        writeBufferToDrum(writeBuffer, drumWriteField, drumAddr, count);

        if( reopened && (drumFd >= 0) )
        {
            if( wbP )
            {
                wbClose(wbP, drumFd);
            }
            else
            {
                close(drumFd);
            }

            drumFd = -1;
        }
    }

    requestPending = ioBusy = dbaPending = false;
    haltedWhileArmed = false;
    teError = false;
    powerCleared = true;
    enablePolling(0);
    iotCondLog(LOG_START, "IOT 61 power clear, %d words of a write kept\n", count);
}

// Called every main-loop pass from the drum's first use, running or halted.
// It notes any halt so the next transfer looks at the file again.
// It also notes a halt while a transfer waits for the drum or a dba break is armed for iotPoll() to act on when
// the machine runs again, the drum keeps turning through a halt.
// The poll interval was set in executed cycles before the halt, so the next poll could come
// after the drum's arrival and start the transfer late so poll on the first cycle instead.
// After a power clear it also clears busy which iotPowerClear() could not reach.
void
iotIOPoll(PDP1 *pdp1P)
{
    if( powerCleared )
    {
        powerCleared = false;
        CKS(pdp1P) &= ~CKS_DRP;
    }

    if( !pdp1P->run )
    {
        lookAtFile = true;
    }

    if( !pdp1P->run && (requestPending || dbaPending) )
    {
        haltedWhileArmed = true;
        haltEndTime = pdp1P->simtime;
        enablePolling(1);
    }
}

// Used to trigger a break, submit an hsc request,  or determine the end of a transfer.
// If a dba break is armed, dbaPending is true; it is checked apart from the transfer states.
// If a transfer is pending, requestPending will be true.
// If a transfer is in progress, ioBusy will be true.
// Every transfer ends with a break on sbsChan, whether it finished or ended in error (H-23 2-21).
void
iotPoll(PDP1 *pdp1P)
{
int stat;
int count;

    // iotPoll() runs only in executed cycles, so the machine has resumed.
    if( haltedWhileArmed )
    {
        haltedWhileArmed = false;
        resumeAfterHalt();
    }

    // In the DBA, DWC, DCL sequence the break is armed alongside a transfer, and the drum reaches
    // the dba address as that transfer starts, so the break cannot wait for the transfer states.
    if( dbaPending && (pdp1P->simtime >= breakTime) )
    {
        dbaPending = false;
        initiateBreak(sbsChan);             // the DEC drum diagnostic seems to use channel 5
        iotCondLog(LOG_BREAK, "IOT 61 break initiated at drum count %o.\n", drumLoc(pdp1P));
    }

    if( requestPending )
    {
        if( pdp1P->simtime < rotationDelayTime )
        {
            // The drum has not yet turned to the transfer's first address.
            // Nothing has moved yet; the transfer is started below once it has.
            // Keep polling until then.
            enablePolling(1);
            return;
        }

        // Drum in position, do the data transfer
        if( (stat = HSCexecute(chanP, &request)) != HSC_BUSY )
        {
            teError = true;
            requestPending = false;
            CKS(pdp1P) &= ~CKS_DRP;         // and not busy
            initiateBreak(sbsChan);
            enablePolling(dbaPending ? 1 : 0);
            iotCondLog(LOG_HSC, "HSCexcute returned %d, failed.\n", stat);
            return;
        }

        iotCondLog(LOG_HSC, "HSCrequest submitted, channel now busy.\n");
        requestPending = false;
        ioBusy = true;
        // Poll every cycle: the steals are spread over the transfer, so an estimate in unstolen
        // cycles would see completion late.
        enablePolling(1);
    }
    else if( ioBusy )
    {
        // Polled every cycle until the channel has moved the last word.
        iotCondLog(LOG_POLL, "iotPoll in ioBusy state\n");

        if( HSCgetStatus(chanP) == HSC_BUSY )
        {
            // not done yet so just keep waiting
            iotCondLog(LOG_POLL, "iotPoll hsc still busy\n");
            enablePolling(1);
            return;
        }

        // This will just complete the hsc request, it won't wait.
        // Continue, Start and each single step reset the channels, so a transfer a halt came
        // in the middle of reports HSC_ABORT.
        // On the hardware hsc the drum's next word went unanswered,
        // so it set its transfer error and dropped the request (H-23 2-21).
        count = transferCount;
        if( HSCwait(chanP) == HSC_ABORT )
        {
            teError = true;
            count = movedWords();
            iotCondLog(LOG_HSC, "HSCwait: aborted by a halt after %d words.\n", count);
        }
        iotCondLog(LOG_HSC, "HSCwait completed.\n");
        CKS(pdp1P) &= ~CKS_DRP;             // and not busy
        ioBusy = false;

        // If writing, copy the words that moved to the drum.
        if( (request.mode & HSC_MODE_FROMMEM) && (count > 0) )
        {
            iotCondLog(LOG_HSC, "iotPoll writing writebuf to drum\n");
            writeBufferToDrum(writeBuffer, drumWriteField, drumAddr, count);
        }

        initiateBreak(sbsChan);
        enablePolling(dbaPending ? 1 : 0);
        iotCondLog(LOG_POLL, "IOT 61 completed transfer.\n");

#ifdef LOG_TOTALTIME
        // update timing stats
        totalTime += pdp1P->simtime - rqstStartTime;
#endif
    }
    else if( dbaPending )
    {
        // Not at the dba address yet, keep polling until we are.
        enablePolling(1);
    }
    else
    {
        iotCondLog(LOG_POLL, "iotPoll() nothing pending, stopping.\n");
        enablePolling(0);
    }
}

// Return the current rotational position of the drum, 0-4095.
// This is determined directly from simtime, so it tracks the CPU's own time base
// exactly, including through throttle pauses, unaffected by how the emulator paces
// wall-clock time to keep up.
// Simtime itself starts from the wall clock at power-on, so the drum's starting position
// is still arbitrary, matching the real hardware.
int
drumLoc(PDP1 *pdp1P)
{
    return( drumLocAt(pdp1P->simtime) );
}

// The drum's rotational position, 0-4095, at a given simtime.
static int
drumLocAt(uint64_t time)
{
    // The drum has 4096 locations per track and takes 8.5us per location, 4096*8.5 usecs per revolution.
    // Compute the number of words since simtime began, modulo 4096.
    return( (int)((time / 8500ULL) % 4096) );
}

// Called after a halt that came while a transfer waited for the drum or a dba break was armed.
// If the drum passed drumAddr during the halt, nothing answered it (H-23 2-19), so the wait
// runs to the drum's next arrival there after the halt.
// A halt that ended before the arrival changes nothing.
// The targets are computed from haltEndTime as dcl and dba compute them from the time they run.
// A full-track transfer starts anywhere, so it is left alone.
static void
resumeAfterHalt(void)
{
int wordCount;

    wordCount = (drumAddr - drumLocAt(haltEndTime) + 4096) % 4096;

    if( requestPending && (transferCount != 4096) && (haltEndTime >= rotationDelayTime) )
    {
        rotationDelayTime = haltEndTime + ((uint64_t)(wordCount + 1) * 8500ULL);
        iotCondLog(LOG_TIME, "halt passed the drum address, transfer waits %d words\n", wordCount + 1);
    }

    if( dbaPending && (haltEndTime >= breakTime) )
    {
        breakTime = haltEndTime + ((uint64_t)((wordCount < 1) ? 1 : wordCount) * 8500ULL);
        iotCondLog(LOG_BREAK, "halt passed the dba address, break waits %d words\n", wordCount);
    }
}

// The number of words a write took from core before it ended, the words at the front of
// writeBuffer the channel filled in.
// The channel fills it in order from the start.
static int
movedWords(void)
{
int count;

    for( count = 0; (count < transferCount) && (writeBuffer[count] != UNMOVED); ++count )
    {
        ;
    }

    return( count );
}

// Do a drum read handling drum wraparound: the words come from the drum's copy in memory.
static void
readDrumToBuffer(
    Word *buffer,       // must be at least 4096, anything over is unused
    int drumField,      // which 4K block on drum
    int drumAddr,       // start point relative to drum index
    int transferCount)  // number of words to transfer
{
int drumSplitCount = 0;
int drumRemainderCount = 0;

    if( (drumAddr + transferCount) > 4095 )
    {
        drumSplitCount = 4096 - drumAddr;   // we transfer this many before wraparound
        drumRemainderCount = transferCount - drumSplitCount;
    }
    else
    {
        drumSplitCount = transferCount;
        drumRemainderCount = 0;
    }

    iotCondLog(LOG_READ, "read drum to buffer, drumSplitCount %d, drumRemainderCount %d\n",
        drumSplitCount, drumRemainderCount);

    memcpy(buffer, &drumImage[(drumField * 4096) + drumAddr], (drumSplitCount * sizeof(Word)));
    if( drumRemainderCount )
    {
        memcpy(buffer + drumSplitCount, &drumImage[drumField * 4096], (drumRemainderCount * sizeof(Word)));
    }
}

// Do a drum write handling drum wraparound: the words go into the drum's copy in memory, and
// each part is queued for the file.
static void
writeBufferToDrum(
    Word *buffer,       // must be at least 4096, anything over is unused
    int drumField,      // which 4K block on drum
    int drumAddr,       // start point relative to drum index
    int transferCount)  // number of words to transfer
{
int drumSplitCount = 0;
int drumRemainderCount = 0;

    if( (drumAddr + transferCount) > 4095 )
    {
        drumSplitCount = 4096 - drumAddr;   // we transfer this many before wraparound
        drumRemainderCount = transferCount - drumSplitCount;
    }
    else
    {
        drumSplitCount = transferCount;
        drumRemainderCount = 0;
    }

    iotCondLog(LOG_WRITE, "write buffer to drum, drumSplitCount %d, drumRemainderCount %d\n",
        drumSplitCount, drumRemainderCount);

    memcpy(&drumImage[(drumField * 4096) + drumAddr], buffer, (drumSplitCount * sizeof(Word)));
    queueWrite(drumField, drumAddr, drumSplitCount);

    if( drumRemainderCount )
    {
        iotCondLog(LOG_WRITE, "writing remainder from buffer location %d to disk offset %o\n", drumSplitCount,
            DRUMADDRTOSEEK(drumField, 0));
        memcpy(&drumImage[drumField * 4096], buffer + drumSplitCount, (drumRemainderCount * sizeof(Word)));
        queueWrite(drumField, 0, drumRemainderCount);
    }
}

// Called by dcl before a transfer uses the drum's copy.
// The first transfer reads the file in.
// If another program closes the file it wrote or renames one over it, the file is read in/ again.
// After a halt, the file is compared with what the load or our own last write left.
// A difference may only be our writes still in the queue, so the queue is drained and the file compared again,
// and a file still different was changed by someone else and is read in again.
// A halt changes nothing far more often and single steps come fast, the compare saves a read
// of the whole file each time.
// No return value.
static void
checkImage(void)
{
    readWatch();
    if( !imageLoaded )
    {
        loadImage();
        lookAtFile = false;
        fileEvent = false;
        return;
    }

    if( fileEvent )
    {
        lookAtFile = false;
        fileEvent = false;
        reloadImage();
        return;
    }

    if( !lookAtFile )
    {
        return;
    }

    lookAtFile = false;
    if( !fileChanged() )
    {
        return;
    }

    if( wbP )
    {
        wbDrain(wbP);
    }

    if( !fileChanged() )
    {
        return;
    }

    reloadImage();
}

// Opens the drum file again,since a file replaced under the same name is another file and reads it in.
// Our writes still in the queue go to the old copy first.
// Our own close raises an event which is dropped, the read that follows it sees everything closed before it.
// The reload is reported only if the drum's contents changed.
// No return value.
static void
reloadImage(void)
{
static Word oldImage[DRUMWORDS];

    if( wbP )
    {
        wbDrain(wbP);
    }

    memcpy(oldImage, drumImage, sizeof(drumImage));
    if( drumFd >= 0 )
    {
        close(drumFd);          // the queue is drained, so nothing is waiting to use it
    }

    drumFd = open(DRUMFILE, O_RDWR + O_CREAT, 0666);
    readWatch();
    fileEvent = false;
    loadImage();
    if( memcmp(oldImage, drumImage, sizeof(drumImage)) != 0 )
    {
        fprintf(stderr, "drum: %s changed on disk; read it again\n", DRUMFILE);
    }
}

// Sets up the inotify watch readWatch() reads.
// The drum's writes go through a descriptor it keeps open, so they never raise IN_CLOSE_WRITE.
// Another program writing the file raises it when it closes, and one renaming a file over the name raises IN_MOVED_TO.
// The watch is on the directory since a rename replaces the file, and on the resolved path's directory, since the
// name may be a symlink.
// Without a watch a change is still seen after a halt.
// No return value.
static void
watchFile(void)
{
char path[PATH_MAX];
char *slashP;

    watchTried = true;
    if( !realpath(DRUMFILE, path) )
    {
        fprintf(stderr, "drum: cannot resolve %s (%s); a change to it is seen only after a halt\n",
            DRUMFILE, strerror(errno));
        return;
    }

    slashP = strrchr(path, '/');
    snprintf(drumName, sizeof(drumName), "%s", (slashP + 1));
    if( slashP == path )
    {
        slashP[1] = '\0';       // the file is in /
    }
    else
    {
        *slashP = '\0';
    }

    watchFd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if( (watchFd < 0) || (inotify_add_watch(watchFd, path, (IN_CLOSE_WRITE | IN_MOVED_TO)) < 0) )
    {
        fprintf(stderr, "drum: cannot watch %s (%s); a change to %s is seen only after a halt\n",
            path, strerror(errno), drumName);
        if( watchFd >= 0 )
        {
            close(watchFd);
            watchFd = -1;
        }
    }
}

// Reads every event waiting on the watch, without blocking.
// An event for the drum file or a lost event has checkImage() read the file in again.
// The drum's own close at STOP raises one too, and costs one read of the file after the next start.
// No return value.
static void
readWatch(void)
{
char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
const struct inotify_event *evP;
ssize_t n;
ssize_t at;

    if( watchFd < 0 )
    {
        return;
    }

    for( ;; )
    {
        n = read(watchFd, buf, sizeof(buf));
        if( (n < 0) && (errno == EINTR) )
        {
            continue;
        }

        if( n <= 0 )
        {
            return;             // EAGAIN: nothing more waiting
        }

        for( at = 0; at < n; at += (ssize_t)(sizeof(struct inotify_event) + evP->len) )
        {
            evP = (const struct inotify_event *)(buf + at);
            if( (evP->mask & IN_Q_OVERFLOW) || ((evP->len > 0) && (strcmp(evP->name, drumName) == 0)) )
            {
                fileEvent = true;
            }
        }
    }
}

// Reads the drum file into the drum's copy in memory.
// A short or unreadable file is a drum whose unwritten words are 0, as a read of them gives.
// Notes the file's state for fileChanged().
// No return value.
static void
loadImage(void)
{
size_t done;
ssize_t n;

    done = 0;
    if( drumFd >= 0 )
    {
        while( done < sizeof(drumImage) )
        {
            n = pread(drumFd, ((char *)drumImage) + done, (sizeof(drumImage) - done), (off_t)done);
            if( (n < 0) && (errno == EINTR) )
            {
                continue;
            }

            if( n <= 0 )
            {
                break;
            }

            done += (size_t)n;
        }
    }

    memset(((char *)drumImage) + done, 0, (sizeof(drumImage) - done));
    imageLoaded = true;

    pthread_mutex_lock(&stampLock);
    if( (drumFd < 0) || (fstat(drumFd, &drumStamp) != 0) )
    {
        memset(&drumStamp, 0, sizeof(drumStamp));
    }
    pthread_mutex_unlock(&stampLock);
}

// Returns true if the drum file is not as the load or our own last write left it: another file
// under the name, a different size, modification time or change time, or no file.
static bool
fileChanged(void)
{
struct stat st;
bool changed;

    if( stat(DRUMFILE, &st) != 0 )
    {
        return(true);
    }

    pthread_mutex_lock(&stampLock);
    changed = (st.st_dev != drumStamp.st_dev) || (st.st_ino != drumStamp.st_ino)
        || (st.st_size != drumStamp.st_size)
        || (st.st_mtim.tv_sec != drumStamp.st_mtim.tv_sec) || (st.st_mtim.tv_nsec != drumStamp.st_mtim.tv_nsec)
        || (st.st_ctim.tv_sec != drumStamp.st_ctim.tv_sec) || (st.st_ctim.tv_nsec != drumStamp.st_ctim.tv_nsec);
    pthread_mutex_unlock(&stampLock);
    return(changed);
}

// Queues count words of the drum's copy, from drumAddr in drumField, for the file.
// With no queue the write is done here.
// No return value.
static void
queueWrite(int drumField, int drumAddr, int count)
{
const char *dataP;
size_t len;
size_t done;
ssize_t n;

    if( drumFd < 0 )
    {
        return;
    }

    dataP = (const char *)&drumImage[(drumField * 4096) + drumAddr];
    len = (size_t)count * sizeof(Word);

    if( wbP )
    {
        wbWrite(wbP, drumFd, dataP, len, (off_t)DRUMADDRTOSEEK(drumField, drumAddr), writeDone, NULL);
        return;
    }

    for( done = 0; done < len; done += (size_t)n )
    {
        n = pwrite(drumFd, dataP + done, (len - done), ((off_t)DRUMADDRTOSEEK(drumField, drumAddr) + (off_t)done));
        if( (n < 0) && (errno == EINTR) )
        {
            n = 0;
            continue;
        }

        if( n <= 0 )
        {
            break;
        }
    }

    writeDone(NULL, drumFd, (done == len));
}

// A queued write is done, normally on the writer thread.
// A failure is reported on stderr once until a write succeeds again.
// A success notes the file's state for fileChanged(), unless the write was to a file the drum has
// since closed.
static void
writeDone(void *argP, int fd, bool ok)
{
struct stat st;

    (void)argP;
    if( !ok )
    {
        if( !writeFailReported )
        {
            fprintf(stderr, "drum: writing %s failed: %s; the file is out of date\n", DRUMFILE, strerror(errno));
            writeFailReported = true;
        }
        return;
    }

    writeFailReported = false;
    if( fstat(fd, &st) != 0 )
    {
        return;
    }

    pthread_mutex_lock(&stampLock);
    if( (st.st_dev == drumStamp.st_dev) && (st.st_ino == drumStamp.st_ino) )
    {
        drumStamp = st;
    }
    pthread_mutex_unlock(&stampLock);
}
