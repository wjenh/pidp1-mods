/**
 * Dynamic IOT for tyi (device 4) -- typewriter input.
 *
 * This is the tyi side of the typewriter, moved from pdp1.c where it didn't belong.
 * tyi is its own IOT, independent of tyo, IOT_3.c, see the notes in it.
 *
 * tyi is completely asynchronous on real PDP-1 hardware and on this emulator,
 * it does NOT honor i or C as designed. Don't use them.
 *
 * 19-Jun-2026 wje initial version.
 */
#include "iotHandler.h"
#include <unistd.h>
//#define DOLOGGING
#include "iotLogger.h" 

// Keep these in sync with common.h if their signatures ever change.
void closefd(FD *fd);
void waitfd(FD *fd);

// Keep in sync with pdp1.c if those values ever change.
#define US(us) ((us)*1000 - 1)
#define TTI_CHAN 7

int
iotHandler(PDP1 *pdp1P, int device, int pulse, int completion)
{
    if(!pulse)
    {
        IO(pdp1P) = 0;
    }
    else
    {
        pdp1P->tbs = 0;
        IO(pdp1P) |= pdp1P->tb;
        iotLog("In iot tyi mb %o dev %o, tb %o\n", MB(pdp1P), device, pdp1P->tb);
    }

    return(1);
}

// Called unconditionally once per main-loop iteration regardless of run state, a
// human can type or a remote client can send a byte at any time, independent of
void
iotIOPoll(PDP1 *pdp1P)
{
char c;

    if(pdp1P->tyo)
    {
        pdp1P->tyi_wait = pdp1P->simtime + US(25000);
    }

    if(!(pdp1P->tyi_wait < pdp1P->simtime && pdp1P->typ_fd.ready))
    {
        return;
    }

    if(read(pdp1P->typ_fd.fd, &c, 1) <= 0)
    {
        closefd(&pdp1P->typ_fd);
        pdp1P->typ_fd.fd = -1;
        iotLog("In iot tyi poll, got EOF on read \n");
        return;
    }

    waitfd(&pdp1P->typ_fd);
    iotLog("In iot tyi poll, got ch %o\n", c);
    pdp1P->tb = c & 077;
    pdp1P->tbs = 1;
    pdp1P->pf |= 040;
    initiateBreak(TTI_CHAN);

    // PDP-1 has to keep up, so avoid clobbering tb before tyi picks it up.
    pdp1P->tyi_wait = pdp1P->simtime + US(25000);
}
