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
 *   - the per-character write-out logic from handleio()'s Punch block, now in iotDeadline()
 *     below
 *
 * The inter-character delay is a deadline on the device time base (iotPollAt() with
 * IOT_TIME_DEVICE, see iotHandler.h): simtime while the power is on, so cycles the drum or
 * the 340 steal and the whole of a mul or div count, and it keeps running in a halt. A
 * throttle lag-cap span does not count, so a host stall never cuts a character short.
 *
 * pdp1.c no longer has a builtin punch: this plugin is the only code that services the
 * punon/pb/pcp fields. What is left in papertape.c's handleio() is the front-panel FEED key's
 * blank feed (tape_feed/feed_time), which punches independent of any IOT.
 *
 * Scope: this plugin only takes over devices 5 and 6. Reader (rpa/rpb, 1/2) and the
 * typewriter (tyo/tyi, 3/4) are plugins of their own.
 *
 * 19-Jun-2026 wje initial version.
 * 24-Sep-2026 Claude a network punch no longer blocks the emulator: a character its socket
 *    cannot take yet is kept and tried again, and the punch stays busy until it is written.
 *    punon is cleared when a character is finished.
 * 28-Sep-2026 Claude the character delay is a simtime deadline, not a count of executed cycles.
 */
#include "iotHandler.h"
#include <errno.h>
#include <unistd.h>

// pdp1.c keeps this as a private #define not exposed via pdp1.h to plugins.
// Keep in sync with pdp1.c if this value ever changes.
#define PUN_CHAN 6

// pdp1.c's PDLY, 63 chars/sec.
#define PUNCH_DELAY_NS USTONS(15873)

// How often a character a network client's socket could not take is tried again.
#define PUNCH_RETRY_NS MSTONS(1)

// pdp1.c's B5/B6 (wait/complete IOT flag bits). Needed here because ppa/ppb's base
// opcode (0730005/0730006) already has B5 baked in -- see the 18-Jun-2026 tyo fix and
// its generalization, the am1 skill's examples.md (async device I/O). The generic nac/
// completion value dynamicIotProcessor() passes in is 0 (no completion) whenever BOTH
// B5 and B6 end up set, exactly what happens for "ppa C"/"ppb C" written naively (C
// OR's onto a base that already has B5). Recomputing pcp from raw MB here (any-bit-set,
// not exact-one-bit-set) avoids that silent hang.
#define B5 010000
#define B6 004000

// Called twice per ppa/ppb IOT instruction (once on TP7 with pulse=0, once on TP10 with
// pulse=1) -- punch is the mirror image of the reader's pulse numbering: TP7 (pulse=0)
// arms a fresh transfer (clears pb, marks punon, and starts the inter-character delay via
// iotPollAt()), while TP10 (pulse=1) records whether a completion pulse was requested
// (pcp) and loads pb from IO -- alphanumeric (device 5, low byte of IO) or binary (device
// 6, channel-8 framed: 0200 | IO bits 6-11). The actual character write-out happens later
// in iotDeadline() below, once the delay has elapsed. pcp (was completion requested) is
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
        iotPollAt(IOT_TIME_DEVICE, (iotTime(IOT_TIME_DEVICE) + PUNCH_DELAY_NS));
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

// Called once PUNCH_DELAY_NS has passed since the arming iotHandler() call above. The
// deadline is one-shot, so nothing is left armed; the next IOT pulse arms it again. Writes the
// assembled byte (pb) to the punch fd if open, signals completion (ios) if a completion pulse
// was requested, and unconditionally requests a sequence break on PUN_CHAN, matching the
// original handleio() Punch block's behavior exactly.
// A network punch is non-blocking (main.c). If its socket is full, the punch has not finished:
// the character is kept and tried again after PUNCH_RETRY_NS, with no completion or break
// until it is written, so a program waits on a stalled client as on a slow punch.
void
iotDeadline(PDP1 *pdp1P)
{
    if(pdp1P->p_fd >= 0)
    {
        char c = pdp1P->pb;
        if( (write(pdp1P->p_fd, &c, 1) < 0) && ((errno == EAGAIN) || (errno == EWOULDBLOCK) || (errno == EINTR)) )
        {
            iotPollAt(IOT_TIME_DEVICE, (iotTime(IOT_TIME_DEVICE) + PUNCH_RETRY_NS));
            return;
        }
    }

    pdp1P->punon = 0;

    if(pdp1P->pcp)
    {
        IOCOMPLETE(pdp1P);
    }

    initiateBreak(PUN_CHAN);
}
