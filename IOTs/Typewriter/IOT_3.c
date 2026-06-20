/**
 * Dynamic IOT for tyo (device 3) -- typewriter output.
 *
 * tyo is its own real handler (unlike Reader/Punch, tyi is NOT an alias of tyo --
 * see IOT_4.c). tyi and tyo are independent input/output streams that can be in
 * flight simultaneously and need different polling mechanisms, so they're two
 * separate real dynamic-IOT entries that happen to share the same underlying
 * pdp1P fields (tb/tbs/tbb/tyo/tcp/tyi_wait/typ_fd/pf) and fd (typ_fd), exactly
 * the way the original built-in code did.
 *
 * This is a faithful port of pdp1.c's tyo logic:
 *   - the arm/load logic from iot_pulse()'s case 003, now in iotHandler()
 *   - the output-completion logic from handleio()'s Typewriter block (the
 *     typ_time-gated half only -- the tyi_wait/typ_fd.ready half belongs to
 *     IOT_4.c), now in iotPoll() below
 *
 * Unlike the original, this uses the cycle-count poll mechanism (enablePolling()/
 * iotPoll(), see IOTs/Clock/IOT_32.c and IOTs/Punch/IOT_6.c) instead of a simtime
 * comparison -- TYODLY (100000us, 10 cps) divides evenly into exactly 20000 cycles
 * at 5us/cycle, the same value already used as the canonical enablePolling()
 * example in IOTs/Demos/IOT_40.c, so there's no rounding error to worry about here
 * (contrast IOTs/Punch/IOT_6.c's PDLY, which doesn't divide evenly and rounds down
 * by a negligible 3us).
 *
 * Case-shift bug fix (19-Jun-2026): the original code detected a "shift" character
 * via `(tb & 076) == 034`, treating 034/035 as the case-shift codes. That's wrong --
 * 034/035 are actually the Black/Red ribbon-color shift codes. The real case-shift
 * codes, confirmed against this project's own bin/decode_fiodec.py,
 * bin/encode_fiodec.py, and Tools/AM1/parsefns.c's concise2ascii table (all three
 * agree), are 072 (octal) = Lcs (lower-case shift) and 074 (octal) = Ucs
 * (upper-case shift). Default startup state is lower case (tbb initialized to 0 in
 * pdp1.c, matches).
 *
 * Red/Black ribbon-color fix (20-Jun-2026): the first pass at the case-shift fix
 * (above) still conflated case and color -- it encoded tbb (case state) into bit 6
 * of every byte written to typ_fd, swallowed 034/035 entirely, and relied on
 * typtelnet.c reading that bit to decide ANSI red/black color. That's a real
 * Flexowriter feature lost: case (Lcs/Ucs, 072/074) and ribbon color (Blk/Red,
 * 034/035) are two genuinely independent shift mechanisms on the real hardware, not
 * one feature wearing two names. The fix is to stop doing any of this here: tb is
 * now forwarded to typ_fd completely raw, unmodified, exactly as the PDP-1 program
 * transmitted it (no bit-packing, no swallowing, no marker byte) -- typtelnet.c's
 * putfio() already has a complete fio2uni[] table with dedicated Lcs/Ucs/Blk/Red
 * sentinel entries (the Blk/Red entries were previously aliased to a generic
 * "ignore" placeholder and never actually implemented; fixed there too, see that
 * file). tbb is still tracked here from 072/074 *only* for the front-panel case
 * light (panel1.c reads pdp->tbb directly, `if(!pdp->tbb) l9 |= 0200000;`) -- it no
 * longer has anything to do with what gets sent to typ_fd.
 *
 * pdp1.c's own (unguarded fallback) copy of this logic was NOT changed for either
 * fix -- flagged to the user as a separate fix candidate, out of scope for this
 * extraction, since it's unreachable once this plugin is loaded.
 *
 * pdp1.c's handleio() has a matching guard (!dynamicIotOwnsDevice(3)) on its own
 * builtin tyo-output block. tyi (device 4, IOT_4.c) is a separate plugin with its
 * own guard on the input half of the same handleio() Typewriter block.
 *
 * 19-Jun-2026 wje initial version.
 * 20-Jun-2026 wje stop conflating case and ribbon-color; forward tb raw.
 */
#include "iotHandler.h"
#include <unistd.h>

// pdp1.c keeps these as private #defines not exposed via pdp1.h to plugins.
// Keep in sync with pdp1.c if those values ever change.
#define TTO_CHAN 8

// pdp1.c's B5/B6 (wait/complete IOT flag bits). Needed here because tyo's base
// opcode (0730003) already has B5 baked in -- this is the original 18-Jun-2026 fix
// (pdp->tcp = !!(MB & (B5|B6)) instead of trusting the generic nac/completion
// value, which comes out 0 whenever both bits end up set, e.g. for "tyo C"). See
// Claude/skill-updates/examples-additions.md for the generalization to ppa/ppb/
// rpa/rpb, which had the identical bug and got the identical fix this session.
#define B5 010000
#define B6 004000

// Cycle-count equivalent of pdp1.c's TYODLY (US(100000), 10 cps). Divides evenly
// at 5us/cycle -- no rounding (contrast Punch's PDLY).
#define TYO_POLL_CYCLES USTOCYCLES(100000)

// Called twice per tyo IOT instruction (once on TP7 with pulse=0, once on TP10 with
// pulse=1). On TP7, clears tb if the typewriter is currently idle (does nothing if a
// character is already mid-print, matching the original "only clear once"
// behavior). On TP10, recomputes tcp from raw MB (not from the completion/nac
// parameter -- see the B5/B6 comment above), and if the typewriter was idle, marks
// it busy, loads tb from IO's low 6 bits, and arms the output delay via
// enablePolling(). The actual character write-out happens later in iotPoll() below.
// Returns 1 always (per the dynamic-IOT contract this means "the IOT was processed").
int
iotHandler(PDP1 *pdp1P, int device, int pulse, int completion)
{
    if(!pulse)
    {
        if(!pdp1P->tyo)
        {
            pdp1P->tb = 0;
        }
    }
    else
    {
        pdp1P->tcp = !!(MB(pdp1P) & (B5 | B6));

        if(!pdp1P->tyo)
        {
            pdp1P->tyo = 1;
            pdp1P->tb |= IO(pdp1P) & 077;
            enablePolling(TYO_POLL_CYCLES);
        }
    }

    return(1);
}

// Called by dynamicIotProcessorDoPoll() once TYO_POLL_CYCLES cycles have elapsed
// since the arming iotHandler() call above. One-shot delay (not free-running), so
// polling is disabled again immediately after firing -- the next tyo IOT re-arms
// it. Updates tbb (the front-panel case-shift light only, see panel1.c) if tb is a
// genuine case-shift code (072 Lcs / 074 Ucs), then forwards tb to typ_fd completely
// raw and unmodified regardless of what it is -- ordinary character, case shift, or
// ribbon-color shift (034 Blk / 035 Red) alike. typtelnet.c's putfio() owns all
// interpretation of the raw code from here (case tracking for glyph selection, color
// tracking for the ANSI escape it forwards to the typewriter client); see that file
// and the file header comment above for why this changed from the first pass at
// this fix. Marks the typewriter idle again, signals completion (ios) if tcp was
// set, and unconditionally requests a sequence break on TTO_CHAN, matching the
// original handleio() Typewriter-output block's behavior. No return value (void).
void
iotPoll(PDP1 *pdp1P)
{
    if(pdp1P->tb == 072 || pdp1P->tb == 074)
    {
        pdp1P->tbb = (pdp1P->tb == 074);
    }

    if(pdp1P->typ_fd.fd >= 0)
    {
        char c = pdp1P->tb;
        write(pdp1P->typ_fd.fd, &c, 1);
    }

    pdp1P->tyo = 0;

    if(pdp1P->tcp)
    {
        IOCOMPLETE(pdp1P);
    }

    initiateBreak(TTO_CHAN);
    enablePolling(0);
}
