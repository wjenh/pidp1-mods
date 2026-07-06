/*
 * This is an implementation of the PDP-1 Type 23 Parallel Drum.
 * The drum data is stored in '/opt/pidp1-mods/pdp23drum' and is a binary image of the drum,
 * stored as 18 bit pdp-1 words per 32 bit image word.
 * The drum also uses IOTs 62 and 63, which alias to this one.
 *
 * wje 20-Jun-2026 - cleanup, begin this revision history, wasn't initially included
 * wje 3-Jul-2026 - set TE error if a write fails
 * wje 6-Jul-2026 - completely rework drum timing, now based on real time, the drum was always spinning,
 *    switch to THREADED mode so real cycle-stealing happens,
 *    change dcl completion timing to account for the cycle-stealing the high speed channel does.
 */

#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <time.h>

#include "highSpeedChannels.h"
#include "iotHandler.h"

#define DOLOGGING
#include "iotLogger.h"
#define LOG_START 0
#define LOG_IOT 0
#define LOG_POLL 0
#define LOG_HSC 0
#define LOG_TIME 0
#define LOG_TOTALTIME 1
#define LOG_BREAK 0
#define LOG_READ 0
#define LOG_WRITE 0

// Flag for busy for the cks instruction.
// DRP set is busy, cleared by operation completion, dia, or dba,
// from the DEC-1-137M diagnostic test program
#define CKS_DRP 0000001

#define HSC_CHAN 1      // drum uses 1

#define DRUMFILE "/opt/pidp1-mods/pdp23drum"
#define DRUMADDRTOSEEK(field, offset) (((field * 4096) + (offset)) * (int)sizeof(Word))

static int drumFd = -1;
static int drumReadField;
static int drumWriteField;
static int drumAddr;
static int sbsChan = 5;
static int draStatusBits = 0;       // use the TE error if a file read or write fails
static uint64_t drumTime;           // actual running time since drum was started, not simtime
static uint64_t cmdCompletionTime;  // absolute now()-scale target time for the current dcl to complete

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
static bool teError;

static HSCChannelP chanP;   // how we get data

static void readDrumToBuffer(int, Word *, int, int, int);
static void writeBufferToDrum(int, Word *, int, int, int);
static int drumLoc(void);
static uint64_t now(void);

int
iotHandler(PDP1 *pdp1P, int dev, int pulse, int completion)
{
int stat;
int chanFlags;
int wordCount;
int transferCount;
HSCRequest request;

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
        ioBusy = 0;      // just to be sure
        CKS(pdp1P) &= ~CKS_DRP;        // and not busy

        readMode = IO(pdp1P) & 0400000;
        writeMode = 0;
        drumAddr = IO(pdp1P) & 07777;
        drumReadField = (IO(pdp1P) >> 12) & 037;

        if( MB(pdp1P) & 02000 )
        {
            // dba, using the interrupt system. reqiest break
            // The break happens when the drum location == the drumAddr
            iotCondLog(LOG_IOT, "dba, break on %o\n", drumAddr);
            if( drumAddr < drumLoc() )  // have to wait for it to come around again on the guitar
            {
                wordCount = 4096 - drumLoc() + drumAddr;
            }
            else
            {
                // Target is at or ahead of the current head position, no wraparound needed,
                // the wait is simply the forward distance from here to there.
                wordCount = drumAddr - drumLoc();
            }

            wordCount = (int)(((float)wordCount * 8500.0) / 5000.0) + 1;    // round up, could end up off by 1, ok
            enablePolling(wordCount);
        }
        
        iotCondLog(LOG_IOT, "dia done, read %o, rfield %o, daddr %o\n", readMode, drumReadField, drumAddr);
        break;

    case 062:            // dwc, drum word count or dra, drum request address
        if( MB(pdp1P) & 02000 )
        {
            // dra, return current drum 'counter' in the IO register, along with status
            IO(pdp1P) = drumLoc();
            if( teError )
            {
                // Manual says we set bits 0 and 2, we use it for any write error.
                IO(pdp1P) |= 0500000;
                teError = false;
            }
            iotCondLog(LOG_IOT, "dra drum count %o\n", drumLoc());
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
        // This is nonstandard behavior.
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
        

        // The manual says mem bank is bits 2, 3, but this isn't correct.
        // The hardware description is.
        // It's adtually bits 2-5 to support up to 16 memory modules.
        memBank = (IO(pdp1P) >> 12) & 017;      // support large memory PDP-1's
        memAddr = IO(pdp1P) & 07777;

        iotCondLog(LOG_IOT, "dcl 63 memBank %o memAddr %o\n", memBank, memAddr);

        // And away we go.
        // For read-write mode, we read data first, then write.
        // This is the sequence defined in the hardware description.
        // Both the drum address and the memory address can wrap around.
        if( !readMode && !writeMode )
        {
            return(0);          // do nothing. An error?
        }

#ifdef LOG_TOTALTIME
        rqstStartTime = now();
        totalRequests++;
#endif
        // The transfer happens immediately, but hsc will do the proper cycle-stealing before done
        chanFlags = HSC_MODE_THREADED | HSC_MODE_UPDATEPANEL;

        if( readMode )
        {
            chanFlags |= HSC_MODE_TOMEM;
            readDrumToBuffer(drumFd, readBuffer, drumReadField, drumAddr, transferCount);
            iotCondLog(LOG_IOT, "dcl 63 read drum to rbuffer\n");
        }

        if( writeMode )
        {
            chanFlags |= HSC_MODE_FROMMEM;
            iotCondLog(LOG_IOT, "dcl 63 requesting write\n");
        }

        wordCount = transferCount;             // figure out how many drum word times this will take

        // Transferring a full mem bank is special, it can start anywhere, no rotational delay
        if( transferCount != 4096 )
        {
            if( drumAddr < drumLoc() )  // have to wait for it to come around again on the guitar
            {
                wordCount += 4096 - drumLoc() + drumAddr;
            }
            else
            {
                // Target is at or ahead of the current head position, no wraparound needed,
                // the wait is simply the forward distance from here to there.
                wordCount += drumAddr - drumLoc();
            }
        }

        // we assume we can proceed, manual says program should check status before calling IOT_61.
        request.mode = chanFlags;
        request.count = transferCount;
        request.memBank = memBank;
        request.memAddr = memAddr;
        request.toBufferP = readBuffer;
        request.fromBufferP = writeBuffer;
        stat = HSCexecute(chanP, &request);

        iotCondLog(LOG_HSC, "HSCexecute returned %d\n", stat);
        // We used threaded, all the data transfer by hsc has completed, no need to wait.
        if( writeMode )
        {
            iotCondLog(LOG_POLL, "iotPoll writing writebuf to drum\n");
            writeBufferToDrum(drumFd, writeBuffer, drumWriteField, drumAddr, transferCount);
        }

        ioBusy = 1;
        // Each drum word takes 8.5us; wordCount here is the rotational latency, if any, plus the transfer itself.
        // This uses a real wall-clock completion time rather than a cycle count, gives pefect timing accuracy.
        cmdCompletionTime = now() + ((uint64_t)wordCount * 8500ULL);
        iotCondLog(LOG_TIME,"Completion target in %lu nsecs\n", cmdCompletionTime - now());
        enablePolling(1);
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
    if( drumFd < 0 )
    {
        drumFd = open(DRUMFILE, O_RDWR + O_CREAT + O_SYNC, 0666);
        iotCondLog(LOG_START, "IOT 61 drumFd = %d\n", drumFd);
    }

    if( chanP == 0 )
    {
        chanP = HSCallocateChannel(HSC_CHAN);
        iotCondLog(LOG_START, "IOT 61 channel allocation %s\n", (chanP)?"ok":"failed");
    }

    // The drum was a separate physical device that was always spinning while on,
    // independent of the PDP-1.
    // We track the drum position based on the start time, which is from the first iotStart() we get.
    if( !drumTime )
    {
        drumTime = now();
    }

#ifdef LOG_TOTALTIME
    totalTime = 0;
    totalWords = 0;
    totalRequests = 0;
#endif
}

void
iotStop()
{
    if( drumFd >= 0 )
    {
        close(drumFd);
        drumFd = -1;
    }

    iotCondLog(LOG_START, "IOT 61 stopped\n");
#ifdef LOG_TOTALTIME
    if( totalWords )
    {
        iotCondLog(LOG_TOTALTIME,"%lu words transferred in %lu usecs, %.2f usec/word, %.2f usec/request\n",
            totalWords,
            totalTime / 1000,
            (float)totalTime / (float)totalWords / 1000.0,
            (float)totalTime / (float)totalRequests / 1000.0);
        iotCondLog(LOG_TOTALTIME,"%d dcl requests, Current drum location %d\n", totalRequests, drumLoc());
    }

    // clear, iotStart() also does this
    totalTime = 0;
    totalWords = 0;
    totalRequests = 0;
#endif
    iotCloseLog();
}

// Used to trigger a break or  determine the end of a transfer
// If a transfer is in progress, ioBusy will be true.
// If just waiting for a drom location because dba was used, it will be false.
void
iotPoll(PDP1 *pdp1P)
{
    if( ioBusy )
    {
        // We will be called when the transfer time is up
        iotCondLog(LOG_POLL, "iotPoll completing\n");

        if( HSCgetStatus(chanP) == HSC_BUSY )
        {
            // can happen if the linux scheduler delays updates, so just keep waiting
            iotCondLog(LOG_POLL, "iotPoll hsc still busy\n");
            return;
        }

        if( now() < cmdCompletionTime )
        {
            // The HSC channel data transfer is done, but it uses PDP-1 5usec cycles.
            // The drum's lower 8.5us/word delay hasn't fully elapsed in real time yet.
            // Keep polling until it has.
            iotCondLog(LOG_POLL, "iotPoll hsc done, still waiting on drum timing\n");
            return;
        }

        HSCwait(chanP);                     // this will just complete the hsd request, won't wait
        CKS(pdp1P) |= CKS_DRP;              // set done status for cks
        iotCondLog(LOG_POLL, "IOT 61 completed timeout.\n");
#ifdef LOG_TOTALTIME
        // update timing stats
        totalTime += now() - rqstStartTime;
#endif
    }
    else
    {
        // We got here because of a dba
        initiateBreak(sbsChan);             // the DEC drum diagnostic seems to use channel 5
        iotCondLog(LOG_BREAK, "IOT 61 break initiated at drum count %o.\n", drumLoc());
    }

    ioBusy = 0;
    enablePolling(0);                       // done for now
}

// Do a drum read handling drum wraparound
static void
readDrumToBuffer(
    int drumFd,         // file descriptor for our 'drum'
    Word *buffer,       // must be at least 4096, anything over is unused
    int drumField,      // which 4K block on drum
    int drumAddr,       // start point relative to drum index
    int transferCount)  // number of words to transfer
{
int wantbytes;
int gotbytes;
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

    lseek(drumFd, DRUMADDRTOSEEK(drumField, drumAddr), SEEK_SET);
    // A read fail is ok, could be an uninitialized drum block.
    // Mem gets buffer content, but the rest of the buffer is set to 0.
    wantbytes = (int)sizeof(Word) * drumSplitCount;
    if( (gotbytes = read(drumFd, buffer, wantbytes)) < 0 )
    {
        gotbytes = 0;
    }

    if( gotbytes != wantbytes )
    {
        memset(buffer + (gotbytes / (int)sizeof(Word)), 0, wantbytes - gotbytes);
    }

    if( drumRemainderCount )
    {
        wantbytes = (int)sizeof(Word) * drumRemainderCount;
        lseek(drumFd, DRUMADDRTOSEEK(drumField, 0), SEEK_SET);
        gotbytes = read(drumFd, buffer + drumSplitCount, wantbytes);

        if( gotbytes != wantbytes )
        {
            memset(buffer + drumSplitCount + (gotbytes / (int)sizeof(Word)), 0, wantbytes - gotbytes);
        }
    }
}

// Do a drum write handling drum wraparound
static void
writeBufferToDrum(
    int drumFd,         // file descriptor for our 'drum'
    Word *buffer,       // must be at least 4096, anything over is unused
    int drumField,      // which 4K block on drum
    int drumAddr,       // start point relative to drum index
    int transferCount)  // number of words to transfer
{
int drumSplitCount = 0;
int drumRemainderCount = 0;
int writeCount;

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

    // An error will set teError true.
    lseek(drumFd, DRUMADDRTOSEEK(drumField, drumAddr), SEEK_SET);
    writeCount = (int)sizeof(Word) * drumSplitCount;
    if( write(drumFd, buffer, writeCount) != writeCount )
    {
        teError = true;
        return;
    }

    if( drumRemainderCount )
    {
        iotCondLog(LOG_WRITE, "writing remainder from buffer location %d to disk offset %o\n", drumSplitCount,
            DRUMADDRTOSEEK(drumField, 0));
        lseek(drumFd, DRUMADDRTOSEEK(drumField, 0), SEEK_SET);
        writeCount = (int)sizeof(Word) * drumRemainderCount;
        if( write(drumFd, buffer, writeCount) != writeCount )
        {
            teError = true;
            return;
        }
    }
}

// Return the current rotational position of the drum, 0-4095.
// This is determined from the actual time from drum start and is as accuracte as we can be.
int
drumLoc()
{
    // The drum has 4096 locations per track and takes 8.5us per location, 4096*8.5 usecs per revolution.
    // Compute the number of words since start modulo 4096.
    return( (int)(((now() - drumTime) / 8500L) % 4096) );
}

// Get the real current time, not simtime.
// Returns time in nanoseconds.
uint64_t
now()
{
uint64_t now;
struct timespec tm;

    clock_gettime( CLOCK_MONOTONIC, &tm );
    now = tm.tv_nsec;
    now += (uint64_t)tm.tv_sec * 1000 * 1000 * 1000;
    return(now);
}
