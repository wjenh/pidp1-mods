#include <unistd.h>
#include <fcntl.h>

#include "common.h"
#include "panel_pidp1.h"
#include "pdp1.h"
#include "highSpeedChannels.h"
#include "iotHandler.h"

#define DOLOGGING
#include "iotLogger.h"
#define LOG_START 0
#define LOG_IOT 0
#define LOG_POLL 0
#define LOG_HSC 0
#define LOG_TIME 0
#define LOG_BREAK 0
#define LOG_READ 0
#define LOG_WRITE 0

// Flag for busy for the cks instruction
// DRP set is busy, cleared by operation completion, dia, or dba
// from the DEC-1-137M diagnostic test program
#define CKS_DRP 0000001

/*
 * This is an implementation of the PDP-1 Type 23 Parallel Drum.
 * It keeps the drum data in a file named 'pdp23drum'.
 * The drum also uses IOTs 62 and 63, so replicate into those.
 */

#define HSC_CHAN 1      // drum uses 1

#define DRUMFILE "/opt/pidp1-mods/pdp23drum"
#define DRUMADDRTOSEEK(field, offset) (((field * 4096) + (offset)) * sizeof(Word))

static int drumFd = -1;
static int drumReadField;
static int drumWriteField;
static int drumAddr;
static int transferCount;
static int drumCount;
static int readMode;
static int writeMode;
static int ioBusy;
static int needBreak;
static int inWait;
static u64 lastSimtime;         // used in the polling code for drumcount updates
static u64 cmdCompletionTime;   // relative to pdp1P->simtime

static int memBank;
static int memAddr;
static Word readBuffer[4096];
static Word writeBuffer[4096];

static int sbsChan = 5;
static HSCChannelP chanP;   // how we get data

static void readDrumToBuffer(int, Word *, int, int, int);
static void writeBufferToDrum(int, Word *, int, int, int);

int
iotHandler(PDP1 *pdp1P, int dev, int pulse, int completion)
{
int stat;
int chanFlags;
Word *memBaseP;
HSCRequest request;

    if( pulse )
    {
        return(1);                  // only on one edge
    }

    iotCondLog(LOG_IOT, "In iot 61 as %o, inWait %d\n", dev, inWait);

    if( drumFd < 0 )
    {
        iotCondLog(LOG_IOT, "In iot 61, no drumFd\n");
        return(0);                 // sorry, some error with the drum file
    }

    lastSimtime = pdp1P->simtime;
    enablePolling(1);
    inWait = completion;            // if nonzero, we will be in IOT wait or completion needed state

    switch( dev )
    {
    case 061:            // dia, drum initial address, in the IO register, or dba, drum break address
        needBreak = ioBusy = 0;             // just to be sure
        pdp1P->cksflags &= ~CKS_DRP;        // and not busy

        readMode = pdp1P->io & 0400000;
        writeMode = 0;
        drumAddr = pdp1P->io & 07777;
        drumReadField = (pdp1P->io >> 12) & 037;

        if( inWait )                    // we don't want to be
        {
            inWait = 0;
            IOCOMPLETE(pdp1P);
        }

        if( pdp1P->mb & 02000 )
        {
            // dba, using the interrupt system. reqiest break
            // The break happens when the drumCount == the drumAddr
            needBreak = 1;
            iotCondLog(LOG_IOT, "dba, break on %o\n", drumAddr);
        }
        
        iotCondLog(LOG_IOT, "dia done, read %o, rfield %o, daddr %o\n", readMode, drumReadField, drumAddr);
        break;

    case 062:            // dwc, drum word count or dra, drum request address
        if( pdp1P->mb & 02000 )
        {
            // dra, return current drum 'counter' in the IO register, along with status
            pdp1P->io = drumCount;
            iotCondLog(LOG_IOT, "dra drum count %o\n", drumCount);
        }
        else
        {
            writeMode = pdp1P->io & 0400000;
            drumWriteField = (pdp1P->io >> 12) & 037;
            transferCount = pdp1P->io & 07777;
            if( !transferCount )
            {
                transferCount = 4096;       // 0 means entire track
            }

            iotCondLog(LOG_IOT, "dwc done, write %d, wfield %d, count %o\n", writeMode, drumWriteField, transferCount);
        }

        if( inWait )                    // we don't want to be
        {
            inWait = 0;
            IOCOMPLETE(pdp1P);
        }
        break;

    case 063:            // dcl, drum core location and dss, drum set sbs
        if( pdp1P->mb & 02000 )
        {
            // enable/disable sbs16
            pdp1P->sbs16 = pdp1P->io & 040;

            stat = sbsChan;

            // change interrupt channel?
            if( pdp1P->io & 020 )
            {
                sbsChan = pdp1P->io & 017;
            }
            iotCondLog(LOG_IOT, "dss called with setting %02o\n", pdp1P->io & 077);
            break;
        }
        
        // The manual says mem bank is bits 2, 3, but this isn't correct.
        // The hardware description is.
        // It's adtually bits 2-5 to support up to 16 memory modules.
        memBank = (pdp1P->io >> 12) & 017;      // support large memory PDP-1's
        memAddr = pdp1P->io & 07777;

        iotCondLog(LOG_IOT, "dcl 63 memBank %o memAddr %o\n", memBank, memAddr);

        // And away we go.
        // For read-write mode, we read data first, then write.
        // This is the sequence defined in the hardware description.
        // Both the drum address and the memory address can wrap around.
        if( !readMode && !writeMode )
        {
            return(0);          // do nothing. An error?
        }

        // We want to manage the delay time ourselves
        chanFlags = HSC_MODE_IMMEDIATE;

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

        pdp1P->cksflags |= CKS_DRP;

        // Transferring a full mem bank is special, it can start anywhere, no rotational delay
        if( transferCount != 4096 )
        {
            if( drumAddr < drumCount )  // have to wait for it to come around again on the guitar
            {
                cmdCompletionTime = 4096 - drumCount + drumAddr;
            }
            else
            {
                cmdCompletionTime = drumCount - drumAddr;
            }
        }
        else
        {
            cmdCompletionTime = 0;
        }

        cmdCompletionTime += transferCount;    // and the actual transfer

        // Each drum word takes 8.5us
        cmdCompletionTime = pdp1P->simtime + (cmdCompletionTime * 8500);

        // we assume we get it, manual says to check status before calling IOT_61.
        pdp1P->hsc = 1;                     // and we have to manage the light

        request.mode = chanFlags;
        request.count = transferCount;
        request.memBank = memBank;
        request.memAddr = memAddr;
        request.toBufferP = readBuffer;
        request.fromBufferP = writeBuffer;
        stat = HSCexecute(chanP, &request);

        iotCondLog(LOG_HSC, "HSCexecute returned %d\n", stat);
        // We used immediate, so all the data transfer by hsc has completed.
        if( writeMode )
        {
            iotCondLog(LOG_POLL, "iotPoll writing writebuf to drum\n");
            writeBufferToDrum(drumFd, writeBuffer, drumWriteField, drumAddr, transferCount);
        }

        iotCondLog(LOG_TIME, "Completion in %d usecs, ioWait %d\n", (cmdCompletionTime - pdp1P->simtime)/1000);
        ioBusy = 1;
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

    needBreak = 0;
    inWait = 0;
    drumCount = 0;  // we don't really know where the hardware would have been, just use 0
}

void
iotStop()
{
    iotCloseLog();

    if( drumFd >= 0 )
    {
        close(drumFd);
        drumFd = -1;
    }
}

// Used to update drumCount, trigger a break,  determine the end of a transfer
void
iotPoll(PDP1 *pdp1P)
{
int hsStatus;

    if( ioBusy )
    {
        if( pdp1P->simtime >= cmdCompletionTime )
        {
            iotCondLog(LOG_POLL, "iotPoll completing\n");
            ioBusy = 0;

            pdp1P->cksflags &= ~CKS_DRP;    // and not busy
            drumCount = (drumAddr + transferCount) % 4096;   // sync up the drum count to match the end of the transfer

            if( inWait )
            {
                iotCondLog(LOG_POLL, "iotPoll posting iocomplete\n");
                inWait = 0;
                IOCOMPLETE(pdp1P);
            }

            pdp1P->hsc = 0;               // and light off, aap's HSC used this also, annoying
            iotCondLog(LOG_POLL, "IOT 61 completed timeout.\n");
            enablePolling(0);                               // done for now
        }
    }
    else
    {
        // The original hardware updated this every 8.5us, be we aren't called with that timing.
        // So the count is updated when simtime % 8500 is zero.
        // This won't be exact, but the longer the time but the higher the count, the more accurate it will be.
        // The worst case will be a 10us interval.

        if( pdp1P->simtime >= (lastSimtime + 8500) )
        {
            lastSimtime = pdp1P->simtime;
            drumCount = ++drumCount % 4096;
        }

        if( needBreak && (drumCount == drumAddr) )
        {
            ioBusy = needBreak = 0;
            pdp1P->cksflags &= ~CKS_DRP;    // and not busy
            initiateBreak(5);               // the DEC drum diagnostic seems to use channel 5
            iotCondLog(LOG_BREAK, "IOT 61 break initiated at drum count %o.\n", drumCount);
        }
    }
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
    // a read fail is ok, could be an uninitialized drum block. Mem gets buffer content.
    // But, the rest of the buffer is set to 0.
    wantbytes = sizeof(Word) * drumSplitCount;
    gotbytes = read(drumFd, buffer, wantbytes);
    if( gotbytes != wantbytes )
    {
        memset(buffer + (gotbytes / sizeof(Word)), 0, wantbytes - gotbytes);
    }

    if( drumRemainderCount )
    {
        wantbytes = sizeof(Word) * drumRemainderCount;
        lseek(drumFd, DRUMADDRTOSEEK(drumField, 0), SEEK_SET);
        gotbytes = read(drumFd, buffer + drumSplitCount, wantbytes);

        if( gotbytes != wantbytes )
        {
            memset(buffer + drumSplitCount + (gotbytes / sizeof(Word)), 0, wantbytes - gotbytes);
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

    lseek(drumFd, DRUMADDRTOSEEK(drumField, drumAddr), SEEK_SET);
    write(drumFd, buffer, sizeof(Word) * drumSplitCount);
    if( drumRemainderCount )
    {
        iotCondLog(LOG_WRITE, "writing remainder from buffer location %d to disk offset %o\n", drumSplitCount,
            DRUMADDRTOSEEK(drumField, 0));
        lseek(drumFd, DRUMADDRTOSEEK(drumField, 0), SEEK_SET);
        write(drumFd, buffer + drumSplitCount, sizeof(Word) * drumRemainderCount);
    }
}
