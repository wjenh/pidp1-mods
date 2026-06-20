/**
 * Dynamic IOT for rpa (device 1) and rpb (device 2) -- read perforated tape.
 *
 * rpa reads 6-bit alphanumeric characters, 3 per word; rpb reads a full 18-bit
 * binary word framed on channel 8 (high bit of each byte marks the start of a
 * frame). This file answers directly for device 2 (rpb); device 1 (rpa) is a
 * trivial alias onto this handler (see IOT_1.c) -- dynamicIotProcessor() still
 * passes the *original* device number through to iotHandler(), so the two are
 * told apart below exactly as the old builtin iot_pulse() switch did.
 *
 * This is a faithful port of the reader logic that used to live in pdp1.c:
 *   - the arm/start logic from iot_pulse()'s case 001/002, now in iotHandler()
 *   - the per-character service logic from handleio()'s Reader block, now in
 *     iotIOPoll() (called once per main-loop iteration regardless of run
 *     state -- see dynamicIotProcessorDoIOPoll() in dynamicIots.c)
 *
 * pdp1.c's handleio() has a matching guard (!dynamicIotOwnsDevice(1)) on its
 * own builtin Reader block so it stops servicing the reader once this plugin
 * is loaded -- without that, both this plugin and the builtin code would try
 * to service the same rcl/r_time/rb/rc/rby/rcp/rbs fields every iteration.
 *
 * Scope: this plugin only takes over devices 1 and 2. Punch (ppa/ppb, 5/6)
 * and typewriter (tyo/tyi, 3/4) are untouched and continue to be serviced by
 * the builtin code in pdp1.c, same as before this plugin existed.
 *
 * 19-Jun-2026 wje initial version.
 */
#include "iotHandler.h"
#include <unistd.h>

// pdp1.c keeps these as private #defines not exposed via pdp1.h to plugins.
// Keep in sync with pdp1.c if those values ever change.
#define US(us) ((us)*1000 - 1)
#define RDLY US(2500)
#define RD_CHAN 1

// pdp1.c's B5/B6 (wait/complete IOT flag bits). Needed here because rpa/rpb's base
// opcode (0730001/0730002) already has B5 baked in -- see the 18-Jun-2026 tyo fix and
// its generalization, Claude/skill-updates/examples-additions.md. The generic nac/
// completion value dynamicIotProcessor() passes in is computed as "MB&(B5|B6)==B5 (xor)
// ==B6", which is 0 (no completion) whenever BOTH bits end up set -- exactly what
// happens for "rpa C"/"rpb C" since C OR's onto a base that already has B5. Recomputing
// rcp from raw MB here (any-bit-set, not exact-one-bit-set) avoids that silent hang.
#define B5 010000
#define B6 004000

// Called twice per rpa/rpb IOT instruction (once on the start-pulse rising edge with pulse=1,
// once on the falling edge with pulse=0). On the rising edge, arms the reader for a fresh
// transfer: records whether a completion pulse was requested (rcp), sets up the alphanumeric
// (3 x 6-bit char, device 1) or binary (1 x 18-bit word, device 8-bit-framed, device 2) framing
// in rby/rc/rcl, and schedules the next character's arrival via r_time. The actual per-character
// transfer work happens later in iotIOPoll() below, once r_time has elapsed. rcp (was
// completion requested) is recomputed from raw MB (any of B5/B6 set), NOT from the
// completion/nac parameter -- see the B5/B6 comment above the #defines, this is the
// same fix tyo needed for the identical baked-in-B5 issue.
// Returns 1 always (per the dynamic-IOT contract this means "the IOT was processed").
int
iotHandler(PDP1 *pdp1P, int device, int pulse, int completion)
{
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

// Called unconditionally once per main-loop iteration (see dynamicIotProcessorDoIOPoll() in
// dynamicIots.c), regardless of run state. If a transfer is armed (rcl set) and its
// inter-character delay (r_time) has elapsed and the reader fd is open, reads one byte from the
// tape, echoes it back (in case r_fd is a socket needing synchronization), and -- once a full
// character has been framed (rc/rby/0200-bit logic, matching the original hardware's STROBE
// PETR/SHIFT RB sequence) -- folds it into rb/IO, signals completion (ios/rbs) and, unless this
// is part of read-in (rim), requests a sequence break on RD_CHAN. No return value (void).
void
iotIOPoll(PDP1 *pdp1P)
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

            // not sure about this, but seems annoying -- preserved verbatim from the
            // builtin code. req(pdp, RD_CHAN) there is initiateBreak(RD_CHAN) here, since
            // req() is private to pdp1.c.
            if(!pdp1P->rim)
            {
                initiateBreak(RD_CHAN);
            }
        }

        pdp1P->rc = (pdp1P->rc + 1) & 3;
    }
}
