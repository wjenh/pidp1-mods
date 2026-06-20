/**
 * Dynamic IOT for tyi (device 4) -- typewriter input.
 *
 * tyi is its own real handler, independent of tyo (device 3, IOT_3.c) -- see the
 * file header comment in IOT_3.c for why these are two separate real entries
 * rather than an alias pair like Reader/Punch.
 *
 * This is a faithful port of pdp1.c's tyi logic:
 *   - the IO-transfer logic from iot_pulse()'s case 004, now in iotHandler()
 *   - the input-servicing logic from handleio()'s Typewriter block (the
 *     tyi_wait/typ_fd.ready half only -- the typ_time-gated output half belongs
 *     to IOT_3.c), now in iotIOPoll() below
 *
 * tyi is completely asynchronous on real PDP-1 hardware and on this emulator --
 * it does NOT honour B5 ("i")/B6 ("C") at all (see Claude/CLAUDE.md's Typewriter
 * section). There is deliberately no tcp/nac handling here, unlike IOT_3.c's tyo.
 *
 * Uses the unconditional iotIOPoll() mechanism (like the Reader, IOTs/Reader/
 * IOT_2.c), NOT the cycle-count iotPoll()/enablePolling() mechanism that IOT_3.c's
 * tyo uses. tyo's delay is fixed (TYODLY, a constant cycle count) so a periodic
 * poll fits naturally; tyi has no fixed delay at all -- it's gated on an actual
 * external readiness flag (typ_fd.ready, set by the polling thread in pollfd.c
 * when data arrives on the socket) plus a stall window (tyi_wait) that only makes
 * sense to re-check continuously, the same situation the Reader was in.
 *
 * pdp1.c's handleio() has a matching guard (!dynamicIotOwnsDevice(4)) on its own
 * builtin tyi-input block. tyo (device 3, IOT_3.c) is a separate plugin with its
 * own guard on the output half of the same handleio() Typewriter block.
 *
 * 19-Jun-2026 wje initial version.
 */
#include "iotHandler.h"
#include "logger.h"      // for logger()/_logger(), the missed-character diagnostic
#include <unistd.h>

// closefd()/waitfd() are declared in src/blincolnlights/common.h (pollfd.c), but
// that header's own FD typedef conflicts with pdp1.h's identical-but-separate one
// when both are included together (FD is already visible via iotHandler.h's chain
// here) -- so they're just declared locally instead of #include "common.h".
// Keep these in sync with common.h if their signatures ever change.
void closefd(FD *fd);
void waitfd(FD *fd);

// pdp1.c keeps these as private #defines not exposed via pdp1.h to plugins.
// Keep in sync with pdp1.c if those values ever change.
#define US(us) ((us)*1000 - 1)
#define TTI_CHAN 7
#define LOG_TYPEWRITER 0

// Called twice per tyi IOT instruction (once on TP7 with pulse=0, once on TP10 with
// pulse=1). tyi never arms anything or waits -- it's a pure snapshot of whatever
// iotIOPoll() below has most recently placed in tb. TP7 clears IO; TP10 clears tbs
// (the "character available" status bit) and copies tb into IO's low bits.
// Returns 1 always (per the dynamic-IOT contract this means "the IOT was processed").
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
    }

    return(1);
}

// Called unconditionally once per main-loop iteration (see
// dynamicIotProcessorDoIOPoll() in dynamicIots.c), regardless of run state -- a
// human can type, or a remote client can send a byte, at any time, independent of
// whether the CPU happens to be running. First, if tyo (IOT_3.c) currently has an
// output character in flight (pdp1P->tyo, the same flag IOT_3.c sets/clears), keeps
// pushing the input stall window (tyi_wait) forward so a fresh keystroke can't
// clobber tb while it's mid-print -- this replaces the original code's
// `typ_time != NEVER` check, which no longer exists now that tyo's delay is a
// cycle-count poll instead of a simtime deadline; pdp1P->tyo is 1 for exactly the
// same duration typ_time used to be non-NEVER, so the substitution is exact. Then,
// once the stall window has passed and the fd is actually ready, reads one byte,
// logs (but does not block on) a missed-character condition if the previous one
// was never picked up via tyi, stores it in tb, sets tbs and PF1 (pf bit 040), and
// requests a sequence break on TTI_CHAN -- matching the original handleio()
// Typewriter-input block exactly. No return value (void).
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
        return;
    }

    waitfd(&pdp1P->typ_fd);

    if(pdp1P->pf & 040)
    {
        logger(LOG_TYPEWRITER, "char missed <%o>\n", pdp1P->tb);
    }

    pdp1P->tb = 0;
    pdp1P->tb |= c & 077;
    pdp1P->tbs = 1;
    pdp1P->pf |= 040;
    initiateBreak(TTI_CHAN);

    // PDP-1 has to keep up, so avoid clobbering tb before tyi picks it up.
    pdp1P->tyi_wait = pdp1P->simtime + US(25000);
}
