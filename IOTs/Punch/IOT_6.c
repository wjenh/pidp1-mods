/**
 * Dynamic IOT for ppa (device 5) and ppb (device 6) -- punch perforated tape.
 *
 * ppa punches an 8-bit alphanumeric character (IO bits 0-7, i.e. IO & 0377) one per IOT;
 * ppb punches an 8-bit binary byte framed with the channel-8 high bit set (0200 | IO bits
 * 6-11). This file answers directly for device 6 (ppb); device 5 (ppa) is a trivial alias
 * onto this handler (see IOT_5.c) -- dynamicIotProcessor() still passes the *original*
 * device number through to iotHandler(), so the two are told apart below exactly as the
 * old builtin iot_pulse() switch did.
 *
 * This is a faithful port of the punch logic that used to live in pdp1.c:
 *   - the arm/load logic from iot_pulse()'s case 005/006, now in iotHandler() below
 *   - the per-character write-out logic from handleio()'s Punch block, now in iotPoll()
 *     below
 *
 * Unlike the Reader extraction, this uses the existing cycle-count poll mechanism
 * (enablePolling()/iotPoll() -- see dynamicIotProcessorDoPoll() in dynamicIots.c, and
 * IOTs/Clock/IOT_32.c for another example) rather than the Reader's unconditional
 * iotIOPoll() mechanism. The punch's inter-character delay is a fixed cycle count, same
 * as the clock's 1ms tick, so there's no need for the every-main-loop-iteration check the
 * reader's variable framing required -- this matches the project's established idiom for
 * fixed periodic/delay-based servicing and avoids polling every cycle unconditionally.
 *
 * pdp1.c's handleio() has a matching guard (!dynamicIotOwnsDevice(5)) on its own builtin
 * Punch block so it stops servicing the punch once this plugin is loaded -- without that,
 * both this plugin and the builtin code would try to service the same punon/p_time/pb
 * fields. The separate tape_feed/feed_time logic in handleio() (the front-panel FEED key,
 * which blank-feeds tape independent of any IOT) is untouched and unaffected either way.
 *
 * Scope: this plugin only takes over devices 5 and 6. Reader (rpa/rpb, 1/2) is its own
 * plugin; typewriter (tyo/tyi, 3/4) is untouched and continues to be serviced by the
 * builtin code in pdp1.c, same as before this plugin existed.
 *
 * 19-Jun-2026 wje initial version.
 */
#include "iotHandler.h"
#include <unistd.h>

// pdp1.c keeps this as a private #define not exposed via pdp1.h to plugins.
// Keep in sync with pdp1.c if this value ever changes.
#define PUN_CHAN 6

// Cycle-count equivalent of pdp1.c's PDLY (US(15873), 63 chars/sec). USTOCYCLES rounds
// down to the nearest 5us cycle, same convention used elsewhere (e.g. IOT_45.c, IOT_7.c).
#define PUNCH_POLL_CYCLES USTOCYCLES(15873)

// pdp1.c's B5/B6 (wait/complete IOT flag bits). Needed here because ppa/ppb's base
// opcode (0730005/0730006) already has B5 baked in -- see the 18-Jun-2026 tyo fix and
// its generalization, Claude/skill-updates/examples-additions.md. The generic nac/
// completion value dynamicIotProcessor() passes in is 0 (no completion) whenever BOTH
// B5 and B6 end up set, exactly what happens for "ppa C"/"ppb C" written naively (C
// OR's onto a base that already has B5). Recomputing pcp from raw MB here (any-bit-set,
// not exact-one-bit-set) avoids that silent hang.
#define B5 010000
#define B6 004000

// Called twice per ppa/ppb IOT instruction (once on TP7 with pulse=0, once on TP10 with
// pulse=1) -- punch is the mirror image of the reader's pulse numbering: TP7 (pulse=0)
// arms a fresh transfer (clears pb, marks punon, and starts the inter-character delay via
// enablePolling()), while TP10 (pulse=1) records whether a completion pulse was requested
// (pcp) and loads pb from IO -- alphanumeric (device 5, low byte of IO) or binary (device
// 6, channel-8 framed: 0200 | IO bits 6-11). The actual character write-out happens later
// in iotPoll() below, once the delay has elapsed. pcp (was completion requested) is
// recomputed from raw MB (any of B5/B6 set), NOT from the completion/nac parameter --
// see the B5/B6 comment above the #defines, this is the same fix tyo needed for the
// identical baked-in-B5 issue.
// Returns 1 always (per the dynamic-IOT contract this means "the IOT was processed").
int
iotHandler(PDP1 *pdp1P, int device, int pulse, int completion)
{
    if(!pulse)
    {
        pdp1P->pb = 0;
        pdp1P->punon = 1;
        enablePolling(PUNCH_POLL_CYCLES);
    }
    else
    {
        pdp1P->pcp = !!(MB(pdp1P) & (B5 | B6));

        if(device == 00005)
        {
            // ppa -- alphanumeric: 8-bit character, low byte of IO
            pdp1P->pb |= IO(pdp1P) & 0377;
        }
        else
        {
            // ppb -- binary: channel-8 framed, high bit set + IO bits 6-11
            pdp1P->pb |= 0200 | ((IO(pdp1P) >> 12) & 077);
        }
    }

    return(1);
}

// Called by dynamicIotProcessorDoPoll() (see dynamicIots.c) once PUNCH_POLL_CYCLES cycles
// have elapsed since the arming iotHandler() call above. This is a one-shot delay, not a
// free-running tick like the clock's, so polling is disabled again immediately after firing
// -- the next IOT pulse re-arms it via enablePolling() in iotHandler(). Writes the assembled
// byte (pb) to the punch fd if open, signals completion (ios) if a completion pulse was
// requested, and unconditionally requests a sequence break on PUN_CHAN, matching the
// original handleio() Punch block's behavior exactly. No return value (void).
void
iotPoll(PDP1 *pdp1P)
{
    if(pdp1P->p_fd >= 0)
    {
        char c = pdp1P->pb;
        write(pdp1P->p_fd, &c, 1);
    }

    if(pdp1P->pcp)
    {
        IOCOMPLETE(pdp1P);
    }

    initiateBreak(PUN_CHAN);
    enablePolling(0);
}
