/**
 * Dynamic IOT for device 1 and device 2: the paper tape reader (rpa/rpb),
 * and on IOT 1, also the Type 550 Microtape.
 *
 * rpa reads 6-bit alphanumeric characters, 3 per word.
 * rpb reads a full 18-bit binary word framed on channel 8,
 * the high bit of each byte marks the start of a frame).
 *
 * This is a port of the reader logic that used to live in pdp1.c, but didn't belong there.
 * The reader's state is still in the PDP1 struct and main.c still opens the tape file (r_fd).
 *
 * The Type 550 Microtape shares device 01 with rpa.
 * its IOTs are 720201,720301-720701 (mse, mlc, mrd, mwr, mrs).
 * An  IOT 1 with sub-device 2-7 is handed to the microttape logic.
 * This is exactly how it was implemented on the original hardware.
 *
 * 19-Jun-2026 wje initial version.
 * 11-Sep-2026 wje/Claude add magtape dispatching
 */
#include "iotHandler.h"
#include "microtape.h"
#include <unistd.h>

// pdp1.c keeps these as private #defines not exposed via pdp1.h to plugins.
// Keep in sync with pdp1.c if those values ever change.
#define US(us) ((us)*1000 - 1)
#define RDLY US(2500)
#define RD_CHAN 1

#define B5 010000
#define B6 004000

static void readerIOPoll(PDP1 *pdp1P);

int
iotHandler(PDP1 *pdp1P, int device, int pulse, int completion)
{
    // The device test must come first: on device 2, MB may be read-in's dio word.
    if( (device == 001) && mtOwnsIot(MB(pdp1P)) )
    {
        return( mtIotHandler(pdp1P, pulse, completion) );
    }

    if(pulse)
    {
        pdp1P->rcp = !!(MB(pdp1P) & (B5 | B6));

        if(device == 001)
        {
            // rpa -- alphanumeric: 6-bit chars, 3 per word
            pdp1P->rby = 0;
            pdp1P->rc = 3;
            pdp1P->rcl ^= 1;
        }
        else
        {
            // rpb -- binary: full 18-bit word, channel-8 framed
            pdp1P->rby = 1;
            pdp1P->rc = 1;
            pdp1P->rcl = 1;
        }

        pdp1P->r_time = pdp1P->simtime + RDLY;
        pdp1P->rb = 0;
    }

    return(1);
}

// Called unconditionally once per main-loop iteration regardless of run state.
// Services the Microtape's drives, then the reader.
// No return value.
void
iotIOPoll(PDP1 *pdp1P)
{
    mtIOPoll(pdp1P);
    readerIOPoll(pdp1P);
}

// Called when this plugin is loaded and whenever the machine goes from halt to run.
// Only the Microtape has anything to do: the first call mounts the tapes in microtapes.txt.
// No return value.
void
iotStart(void)
{
    mtStart();
}

// Called when the machine halts.
// The Microtape writes any partly written block.
// No return value.
void
iotStop(void)
{
    mtStop();
}

// Called on SIGHUP, after pidp1.config has been reloaded.
// The Microtape rereads microtapes.txt and its break channel.
// No return value.
void
iotUpdate(void)
{
    mtUpdate();
}

// The reader's per-main-loop-iteration service.
// If a transfer is armed (rcl set) and its
// inter-character delay (r_time) has elapsed and the reader fd is open, reads one byte from the
// tape, echoes it back (in case r_fd is a socket needing synchronization), and once a full
// character has been framed folds it into rb/IO, signals completion (ios/rbs) and, unless this
// is part of read-in (rim), requests a sequence break on RD_CHAN.
// No return value (void).
static void
readerIOPoll(PDP1 *pdp1P)
{
uint8_t c;

    if(!(pdp1P->rcl && pdp1P->r_time < pdp1P->simtime && pdp1P->r_fd >= 0))
    {
        return;
    }

    pdp1P->r_time = pdp1P->simtime + RDLY;

    if(read(pdp1P->r_fd, &c, 1) <= 0)
    {
        close(pdp1P->r_fd);
        pdp1P->r_fd = -1;
        return;
    }

    // write back in case this is over a socket and we need to synchronize
    write(pdp1P->r_fd, &c, 1);

    if(pdp1P->rc && (!pdp1P->rby || (c & 0200)))
    {
        // STROBE PETR
        pdp1P->rcl = 0;
        pdp1P->rb |= c & (pdp1P->rby ? 077 : 0377);

        // SHIFT RB
        if(pdp1P->rc != 3)
        {
            pdp1P->rb = (pdp1P->rb << 6) & WORDMASK;
            pdp1P->rcl = 1;
        }

        // CLR IO
        if((pdp1P->rc == 3) && (pdp1P->rcp || pdp1P->rim))
        {
            IO(pdp1P) = 0;
        }

        // +1 RC
        if(pdp1P->rc == 3)
        {
            // READER RETURN
            if(pdp1P->rcp)
            {
                pdp1P->ios = 1;
            }
            else
            {
                pdp1P->rbs = 1;
            }

            if(pdp1P->rcp || pdp1P->rim)
            {
                IO(pdp1P) |= pdp1P->rb;
                pdp1P->rbs = 0;

                if(pdp1P->rim)
                {
                    pdp1P->rim_return = 2;
                }
            }

            if(!pdp1P->rim)
            {
                initiateBreak(RD_CHAN);
            }
        }

        pdp1P->rc = (pdp1P->rc + 1) & 3;
    }
}
