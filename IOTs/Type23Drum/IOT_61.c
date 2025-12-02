#include <unistd.h>
#include <fcntl.h>

#include "common.h"
#include "pdp1.h"
#include "highSpeedChannels.h"
#include "iotHandler.h"

// #define DOLOGGING
#include "Logger/iotLogger.h"

// Flag for busy for the cks instruction
// DRP set is busy, cleared by operation completion, dia, or dba
// from the DEC-1-137M diagnostic test program
#define CKS_DRP 0000001

/*
 * This is an implementation of the PDP-1 Type 23 Parallel Drum.
 * It keeps the drum data in a file named 'pdp23drum'.
 * The drum also uses IOTs 62 and 63, so replicate into those.
 */

#define DRUMFILE "/opt/pidp1/pdp23drum"
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

static void readDrumToBuffer(int, Word *, int, int, int);
static void writeBufferToDrum(int, Word *, int, int, int);

int
iotHandler(PDP1 *pdp1P, int dev, int pulse, int completion)
{
int stat;
int chanFlags;
Word *memBaseP;

    if( pulse )
    {
        return(1);                  // only on one edge
    }

    iotLog("In iot 61 as %o\n", dev);

    if( drumFd < 0 )
    {
        iotLog("In iot 61, no drumFd\n");
        return(0);                 // sorry, some error with the drum file
    }

    lastSimtime = pdp1P->simtime;
    enablePolling(1);
    inWait = completion;            // if nonzero, we will be in IOT wait state

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
            iotLog("dba, break on %o\n", drumAddr);
        }
        
        iotLog("dia done, read %d, rfield %d, daddr %d\n", readMode, drumReadField, drumAddr);
        break;

    case 062:            // dwc, drum word count or dra, drum request address
        if( pdp1P->mb & 02000 )
        {
            // dra, return current drum 'counter' in the IO register, along with status
            pdp1P->io = drumCount;
            iotLog("dra drum count %o\n", drumCount);
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

            iotLog("dwc done, write %d, wfield %d, count %o\n", writeMode, drumWriteField, transferCount);
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
            iotLog("dss called with setting %02o\n", pdp1P->io & 077);
            break;
        }
        
        // The manual says mem bank is bits 2, 3, but this isn't correct.
        // The hardware description is.
        // It's adtually bits 2-5 to support up to 16 memory modules.
        memBank = (pdp1P->io >> 12) & 037;      // support large memory -1's
        memAddr = pdp1P->io & 07777;

        iotLog("dcl 63 memBank %o memAddr %o\n", memBank, memAddr);

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
            iotLog("dcl 63 read drum to rbuffer\n");
        }

        if( writeMode )
        {
            chanFlags |= HSC_MODE_FROMMEM;
            iotLog("dcl 63 requesting write\n");
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

        cmdCompletionTime += transferCount;    // and the actual transfer

        // Each drum word takes 8.5us, plus the rotation time to get to the word.
        cmdCompletionTime = pdp1P->simtime + (cmdCompletionTime * 8500);

        // we assume we get it, manual says to check status before calling IOT_61.
        pdp1P->hsc = 1;                     // and we have to manage the light
        stat = HSC_request_channel(pdp1P, 1, chanFlags, transferCount, memBank, memAddr, readBuffer, writeBuffer);
        iotLog("HSC_request_channel returned %d\n", stat);
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
    iotLog("IOT 61 started\n");
    if( drumFd < 0 )
    {
        drumFd = open(DRUMFILE, O_RDWR + O_CREAT + O_SYNC, 0666);
        iotLog("IOT 61 drumFd = %d\n", drumFd);
    }

    needBreak = 0;
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
        hsStatus = HSC_get_status(1);

        if( (pdp1P->simtime >= cmdCompletionTime) && (hsStatus != HSC_BUSY) )
        {
            iotLog("iotPoll completing, status %d\n", hsStatus);
            ioBusy = 0;

            // We now have original memory contents in writeBuffer, update drum
            if( writeMode )
            {
                iotLog("iotPoll writing writebuf to drum\n");
                writeBufferToDrum(drumFd, writeBuffer, drumWriteField, drumAddr, transferCount);
            }

            pdp1P->cksflags &= ~CKS_DRP;    // and not busy
            drumCount = (drumAddr + transferCount) % 4096;   // sync up the drum count to match the end of the transfer

            if( inWait )
            {
                inWait = 0;
                IOCOMPLETE(pdp1P);
            }

            pdp1P->hsc = 0;
            iotLog("IOT 61 completed timeout.\n");
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
            iotLog("IOT 61 break initiated at drum count %o.\n", drumCount);
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

    iotLog("read drum to buffer, drumSplitCount %d, drumRemainderCount %d\n", drumSplitCount, drumRemainderCount);

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

    iotLog("write buffer to drum, drumSplitCount %d, drumRemainderCount %d\n", drumSplitCount, drumRemainderCount);

    lseek(drumFd, DRUMADDRTOSEEK(drumField, drumAddr), SEEK_SET);
    write(drumFd, buffer, sizeof(Word) * drumSplitCount);
    if( drumRemainderCount )
    {
        iotLog("writing remainder from buffer location %d to disk offset %o\n", drumSplitCount,
            DRUMADDRTOSEEK(drumField, 0));
        lseek(drumFd, DRUMADDRTOSEEK(drumField, 0), SEEK_SET);
        write(drumFd, buffer + drumSplitCount, sizeof(Word) * drumRemainderCount);
    }
}
