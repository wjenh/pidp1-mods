#include <unistd.h>
#include <fcntl.h>

#include "common.h"
#include "pdp1.h"
#include "iotHandler.h"

// #define DOLOGGING
#include "Logger/iotLogger.h"

// flags for busy, done for the cks instruction
// DRP is busy, DRM is done
// from the simh PDP-1 drum implementation
#define CKS_DRP 0400000
#define CKS_DRM 0000040

/*
 * This is an implementation of the PDP-1 Type 23 Parallel Drum.
 * It keeps the drum data in a file named 'pdp23drum'.
 * The drum also uses IOTs 62 and 63, so replicate into those.
 */

#define DRUMFILE "/tmp/pdp23drum"
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
static int errFlags;
static u64 lastSimtime;         // used in the polling code for drumcount updates
static u64 cmdCompletionTime;   // relative to pdp1->simtime

static int memBank;
static int memAddr;

int
iotHandler(PDP1 *pdp1P, int dev, int pulse, int completion)
{
int i;
Word readBuf[4096];                 // needed for read/write mode

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

    switch( dev )
    {
    case 061:            // dia, drum initial address, in the IO register, or dba, drum break address
        if( pdp1P->mb & 02000 )
        {
            // dba, using the interrupt system. reqiest break
            // The break happens when the drumCount == the drumAddr
            needBreak = 1;
        }

        ioBusy = 0;             // just to be sure
        pdp1P->cksflags &= ~(CKS_DRP | CKS_DRM);    // and not busy or done

        readMode = pdp1P->io & 0400000;
        writeMode = 0;
        drumAddr = pdp1P->io & 07777;
        drumReadField = (pdp1P->io & 0370000) >> 12;
        errFlags = 0;
        pdp1P->cksflags &= ~CKS_DRP;     // not busy yet
        
        iotLog("dia done, read %d, rfield %d, daddr %d\n", readMode, drumReadField, drumAddr);
        break;

    case 062:            // dwc, drum word count or dra, drum request address
        if( pdp1P->mb & 02000 )
        {
            // dra, return current drum 'counter' in the IO register, along with status
            pdp1P->io = drumCount | errFlags;
        }
        else
        {
            writeMode = pdp1P->io & 0400000;
            drumWriteField = (pdp1P->io >> 12) & 037;
            drumWriteField = (pdp1P->io & 0370000) >> 12;
            transferCount = pdp1P->io & 07777;

            if( (drumAddr + transferCount) > 4096 )
            {
                errFlags = 0500000;     // Err TE
                return(1);              // do nothing. Is this correct?
            }

            iotLog("dwc done, write %d, wfield %d, count %o\n", writeMode, drumWriteField, transferCount);
            pdp1P->cksflags &= ~CKS_DRM;    // not done now
        }
        break;

    case 063:            // dcl, drum core location
        memBank = (pdp1P->io >> 14) & 03;
        memAddr = pdp1P->io & 017777;

        iotLog("dcl memBank %o memAddr %o\n", memBank, memAddr);
        if( (memAddr + transferCount) > 4096 )
        {
            errFlags = 0500000;     // Err TE
            return(1);              // do nothing. Is this correct?
        }
        else
        {
            errFlags = 0;
        }

        Word *memP = &pdp1P->core[(memBank * 4096) + memAddr];

        // and away we go
        // We might have read/write mode, in which case we have to read the dagta first
        if( readMode && writeMode )
        {
            // we have to read first, then write, then transfer to mem
            lseek(drumFd, DRUMADDRTOSEEK(drumReadField, drumAddr), SEEK_SET);
            // a read fail is ok, could be an uninitialized drum block
            read(drumFd, readBuf, sizeof(Word) * transferCount);
        }

        if( readMode && !writeMode )            // just a regular read
        {
            lseek(drumFd, DRUMADDRTOSEEK(drumReadField, drumAddr), SEEK_SET);
            // a read fail is ok, could be an uninitialized drum block
            read(drumFd, memP, sizeof(Word) * transferCount);
        }
        else if( writeMode )
        {
            lseek(drumFd, DRUMADDRTOSEEK(drumWriteField, drumAddr), SEEK_SET);
            if( write(drumFd, memP, sizeof(Word) * transferCount) < 0)
            {
                return(0);                  // sorry
            }

            if( readMode )
            {
                // now write the buffer to mem
                memcpy(memP, readBuf, sizeof(Word) * transferCount);
            }
        }
        else
        {
            // hmm, not read or write. Do what?
            return(1);
        }

        ioBusy = 1;
        pdp1P->cksflags |= CKS_DRP;
        cmdCompletionTime = transferCount;

        if( drumAddr < drumCount )  // have to wait for it to come around again on the guitar
        {
            cmdCompletionTime += 4096 - drumCount + drumAddr;
        }
        else
        {
            cmdCompletionTime += drumCount - drumAddr;
        }

        cmdCompletionTime = pdp1P->simtime + cmdCompletionTime * 8500;
        iotLog("dcl done\n");
        break;

    default:
        return(0);                // should never happen
    }

    return(1);
}

void iotStart()
{
    iotLog("IOT 61 started\n");
    if( drumFd < 0 )
    {
        drumFd = open(DRUMFILE, O_RDWR + O_CREAT + O_SYNC, 0666);
        iotLog("IOT 61 drumFd = %d\n", drumFd);
    }
    needBreak = 0;
}

void iotStop()
{
    iotCloseLog();

    if( drumFd >= 0 )
    {
        close(drumFd);
        drumFd = -1;
    }
}

// Used to update drumCount, trigger a break,  determine the end of a transfer
void iotPoll(PDP1 *pdp1P)
{
    if( ioBusy )
    {
        if( pdp1P->simtime >= cmdCompletionTime )
        {
            ioBusy = 0;
            pdp1P->cksflags |= CKS_DRM;     // done
            pdp1P->cksflags &= ~CKS_DRP;    // and not busy
            drumCount = drumAddr + transferCount;   // sync up the drum count to match the end of the transfer
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
            pdp1P->cksflags |= CKS_DRM;     // done
            pdp1P->cksflags &= ~CKS_DRP;    // and not busy
            initiateBreak(0);           // no channel specified, what to use for sbs16? Is 0 correct??
            iotLog("IOT 61 break initiated.\n");
        }
    }
}
