#include <unistd.h>
#include <fcntl.h>

#include "common.h"
#include "pdp1.h"
#include "iotHandler.h"

#define DOLOGGING
#include "Logger/iotLogger.h"

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
static u64 cmdStartTime;
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

    enablePolling(1);

    switch( dev )
    {
    case 061:            // dia, drum initial address, in the IO register, or dba, drum break address
        if( pdp1P->mb & 02000 )
        {
            // dba, using the interrupt system. reqiest break
            // THe break happens when the drumCount == the drumAddr
            IONOWAIT(pdp1P); // we don't want to hold for completion
            needBreak = 1;
        }

        readMode = pdp1P->io & 0400000;
        writeMode = 0;
        drumReadField = (pdp1P->io >> 12) & 037;
        drumAddr = pdp1P->io & 07777;
        errFlags = 0;
        break;

    case 062:            // dwc, drum word count or dra, drum request address
        if( (pdp1P->mb & 012000) == 012000 )
        {
            // dra, return current drum 'counter' in the IO register, along with status
            // Since we don't actually have a rotating drum, just return the next loc after the last operation
            IONOWAIT(pdp1P); // we don't want to hold for completion
            pdp1P->io = drumCount | errFlags;
        }
        else
        {
            writeMode = pdp1P->io & 0400000;
            drumWriteField = (pdp1P->io >> 12) & 037;
            transferCount = pdp1P->io & 07777;

            if( (drumAddr + transferCount) > 4096 )
            {
                errFlags = 0500000;     // Err TE
                return(1);              // do nothing. Is this correct?
            }
        }
        break;

    case 063:            // dcl, drum core location
        memBank = (pdp1P->io >> 14) & 03;
        memAddr = pdp1P->io & 017777;

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
            iotLog("063, read and write\n");
            // we have to read first, then write, then transfer to mem
            iotLog("063, read seek %o\n", DRUMADDRTOSEEK(drumReadField, drumAddr));
            lseek(drumFd, DRUMADDRTOSEEK(drumReadField, drumAddr), SEEK_SET);
            // a read fail is ok, could be an uninitialized drum block
            iotLog("063, read %d bytes\n", sizeof(Word) * transferCount);
            read(drumFd, readBuf, sizeof(Word) * transferCount);
        }

        if( readMode && !writeMode )            // just a regular read
        {
            iotLog("063, read only\n");
            iotLog("063, read seek %o\n", DRUMADDRTOSEEK(drumReadField, drumAddr));
            lseek(drumFd, DRUMADDRTOSEEK(drumReadField, drumAddr), SEEK_SET);
            // a read fail is ok, could be an uninitialized drum block
            iotLog("063, read %d bytes to %o\n", sizeof(Word) * transferCount, memP - pdp1P->core);
            read(drumFd, memP, sizeof(Word) * transferCount);
        }
        else if( writeMode )
        {
            iotLog("063, write\n");
            iotLog("063, write seek %o\n", DRUMADDRTOSEEK(drumReadField, drumAddr));
            lseek(drumFd, DRUMADDRTOSEEK(drumWriteField, drumAddr), SEEK_SET);
            iotLog("063, write %d bytes from %o\n", sizeof(Word) * transferCount, memP - pdp1P->core);
            if( write(drumFd, memP, sizeof(Word) * transferCount) < 0)
            {
                return(0);                  // sorry
            }

            if( readMode )
            {
                // now write the buffer to mem
                iotLog("063 read write completion\n");
                iotLog("063, copying %d bytes  to %o\n", sizeof(Word) * transferCount, memP - pdp1P->core);
                memcpy(memP, readBuf, sizeof(Word) * transferCount);
            }
        }
        else
        {
            // hmm, not read or write. Do what?
            return(1);
        }

        ioBusy = 1;
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
}

void iotStop()
{
    iotCloseLog();

    if( drumFd >= 0 )
    {
        close(drumFd);
        drumFd = -1;
        iotLog("IOT 61 drumFd closed\n");
    }
}

// Used to update drumCount, trigger a break,  determine the end of a transfer
void iotPoll(PDP1 *pdp1P)
{
    if( ioBusy )
    {
        if( needBreak && (drumCount == drumAddr) )
        {
            needBreak = 0;
            initiateBreak(0);           // no channel specified, what to use for sbs16? Is 0 correct??
        }
        else if( pdp1P->simtime >= cmdCompletionTime )
        {
            ioBusy = 0;
            drumCount = drumAddr + transferCount;   // sync up the drum count to match the end of the transfer
        }
    }
    else
    {
        drumCount++;
        drumCount %= 4096;      // not quite correct, should be every 8.5 us
    }
}
