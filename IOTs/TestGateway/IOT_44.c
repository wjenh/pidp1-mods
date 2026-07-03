/*
 * IOT 44 -- "Test Gateway": a test-only IOT that exposes the internal High Speed Channel
 * (HSC, src/blincolnlights/pdp1/highSpeedChannels.c) API directly to a PDP-1 assembly
 * program.
 *
 * Architectural scope: normally HSC is never reachable from user code -- it is a private
 * interface used only by device IOTs (IOTs/Type23Drum/IOT_61.c, IOTs/Type340Display/
 * type340emu.c) to move data to/from core memory on their own behalf. There is no PDP-1
 * hardware IOT for HSC itself (the real Type 19 "had no interface from the user side", per
 * Docs/UsingHighSpeedChannels.md). This plugin exists purely so an am1 test program
 * (IOTs/TestGateway/Tests/*.am1) can drive HSCallocateChannel()/HSCfreeChannel()/
 * HSCexecute()/HSCwait()/HSCgetStatus()/HSCreset() directly and verify their behavior --
 * mode semantics, data movement correctness, address wraparound, error handling, and
 * (for normal/cycle-stealing mode specifically) channel priority arbitration -- none of
 * which has any other automated coverage. See Docs/UsingHighSpeedChannels.md for the
 * HSC API contract this plugin is testing against, and src/blincolnlights/pdp1/CLAUDE.md /
 * IOTs/Type340Display/CLAUDE.md for the history of bugs already found and fixed in that
 * module (02-Jul-26 conformance review).
 *
 * Device number: 044 (octal), chosen arbitrarily from the unused range in
 * IOTs/KnownIOTs.txt. Sub-commands are dispatched on the "ch" field (MB bits 6-11, i.e.
 * (MB>>6)&077), the same convention IOTs/DCS2/IOT_22.c uses to multiplex many commands
 * onto one device number. See Am1Includes/HSC/hscgatewaydefs.ah for the am1-side mnemonics
 * (hga/hgf/hgx/hgs/hgw/hgl/hgd/hgr) and their exact encodings.
 *
 * Design constraint that shapes every command here: iotHandler() executes synchronously,
 * inline, on the MAIN emulator thread, as part of a single cycle()/iot() call (see
 * src/blincolnlights/pdp1/CLAUDE.md's "Machine Cycle Structure" section). HSCreset() and
 * every HSC_MODE_IMMEDIATE/HSC_MODE_THREADED path is safe to call directly from here because
 * they either return immediately or perform a single bounded busy-wait (HSCwait() for a
 * THREADED request). Calling HSCwait() on a channel that is still busy from a NORMAL-mode
 * (neither IMMEDIATE nor THREADED) request would be unsafe here: HSCwait() would sem_wait()
 * for HSCdone() to be posted by processChannel(), which is only ever driven by
 * processHSCchannels() from main.c's OWN main-loop iteration -- the same thread this
 * iotHandler() call is blocking. That would deadlock the entire emulator permanently. For
 * this reason, NORMAL-mode completion in the test programs is always observed by polling
 * hgs (HSCgetStatus(), which never blocks) in a loop, exactly like every other device IOT
 * in this codebase polls a busy/ready bit (e.g. Type23Drum's cks-based busy poll). hgw
 * (HSCwait()) is still provided, for testing HSCwait()'s own contract, but test programs
 * must only call it once hgs has already reported HSC_DONE (or for HSC_MODE_IMMEDIATE /
 * HSC_MODE_THREADED requests, where HSCwait() is always safe -- see the per-command
 * comments below).
 *
 * Buffering model: every HSCRequest needs a plugin-owned C buffer distinct from PDP-1 core
 * memory (exactly like the Type 23 Drum's readBuffer/writeBuffer in IOT_61.c) -- HSC moves
 * data between core and a caller-supplied buffer, never core-to-core directly in one call.
 * This plugin keeps one pair of fixed 4096-word buffers per channel slot (toBuf/fromBuf,
 * indexed 1-5 to match HSC's own 1-based channel numbering). hgl ("load scratch") and hgd
 * ("dump scratch") are plain, synchronous memcpy-style helpers -- NOT HSC operations
 * themselves -- that let a test program stage known data into toBuf from its own core
 * memory before an HSC_MODE_TOMEM transfer, and retrieve fromBuf into core after an
 * HSC_MODE_FROMMEM transfer, using only ordinary lac/dac/sad instructions to prepare and
 * verify data. They replicate HSC's own within-bank address wraparound so tests that
 * exercise wraparound behave consistently whether the wrap happens inside HSCexecute()
 * itself or inside one of these helpers.
 *
 * Register-width gotcha: HSC_ERR (src/blincolnlights/pdp1/highSpeedChannels.h) is a plain
 * C int, -1, returned as-is by HSCexecute()/HSCwait()/HSCgetStatus()/HSCfreeChannel() on
 * failure. The pdp1 struct's io/ac fields are "Word" (uint32_t), a 32-bit C type standing
 * in for an 18-bit PDP-1 register, and no existing device IOT had ever needed to put a
 * negative HSC status directly into one before this plugin. A plain "IO(pdp1P) = HSC_ERR;"
 * would sign-extend -1 into all 32 bits of the Word, not the 18 actually meaningful ones.
 * Worse, it would NOT match what an am1 test program gets from writing "[-1]" in its own
 * source: am1 emulates the PDP-1's actual 1's-complement arithmetic for constant folding
 * (see am1-syntax.md), so am1's own "-1" assembles to ~1 = 0777776 octal, not the C/2's-
 * complement all-ones pattern 0777777 a naive unmasked C assignment would produce. To keep
 * this plugin's behavior well-defined and independent of either convention, every write to
 * IO or AC here goes through setReg() (below), which masks with WORDMASK first, so HSC_ERR
 * always shows up as exactly octal 0777777 -- see the HSC_ERR definition and comment in
 * Am1Includes/HSC/hscgatewaydefs.ah, which test programs should compare against literally
 * rather than writing "[-1]" and expecting it to match.
 *
 * 02-Jul-2026 wje/claude -- initial version, written as part of the HSC conformance
 *    review test-suite task.
 */

#include <string.h>

#include "iotHandler.h"
#include "highSpeedChannels.h"

#define DOLOGGING
#include "iotLogger.h"
#define LOG_IOT 0

// Sub-command values, extracted from MB bits 6-11 -- see Am1Includes/HSC/hscgatewaydefs.ah
// for the am1-side "iot ccdd" encodings these correspond to (cc = the value below, dd = 044).
#define CMD_ALLOC   000     // hga -- HSCallocateChannel()
#define CMD_FREE    001     // hgf -- HSCfreeChannel()
#define CMD_EXEC    002     // hgx -- HSCexecute()
#define CMD_STATUS  003     // hgs -- HSCgetStatus()
#define CMD_WAIT    004     // hgw -- HSCwait()
#define CMD_LOAD    005     // hgl -- load scratch buffer from core
#define CMD_DUMP    006     // hgd -- dump scratch buffer to core
#define CMD_RESET   007     // hgr -- HSCreset() (simulated abort)

// Highest legal HSC channel number (matches NUMCHANS in highSpeedChannels.c). Channel
// numbers are 1-based, matching HSC_CHAN usage in IOT_61.c/type340emu.c; index 0 of each
// array below is unused (kept only so channel numbers can index directly, no off-by-one).
#define HG_NUMCHANS 5

// One HSCChannelP per possible channel number, NULL if not currently allocated by this
// plugin. A NULL entry does not necessarily mean the channel is free -- another device
// (the drum uses channel 1, Type 340 Display uses channel 3) may hold it; HSCallocateChannel()
// itself is the source of truth and will fail (return NULL) if so.
static HSCChannelP handles[HG_NUMCHANS + 1];

// Plugin-owned scratch buffers, one read (fromBuf) and one write (toBuf) buffer per channel
// slot, sized to the maximum single-request transfer count (4096 words, one full bank).
static Word toBuf[HG_NUMCHANS + 1][4096];
static Word fromBuf[HG_NUMCHANS + 1][4096];

static bool isValidChanNo(int chanNo);
static bool isAllocatedChanNo(int chanNo);
static int  loadScratch(PDP1 *pdp1P, int chanNo, int cbAddr);
static int  dumpScratch(PDP1 *pdp1P, int chanNo, int cbAddr);
static void setIO(PDP1 *pdp1P, int value);

// Writes 'value' into the IO register, masking to 18 bits first -- see the register-width
// gotcha in the file header comment above. Every place in this file that stores an
// HSC_OK/HSC_BUSY/HSC_DONE/HSC_ABORT/HSC_ERR value (or any other plugin-generated status)
// into IO must go through this function rather than assigning IO(pdp1P) directly, so a
// negative C status always comes out as a well-defined, documented 18-bit bit pattern.
// No return value.
static void
setIO(PDP1 *pdp1P, int value)
{
    IO(pdp1P) = ((Word)value) & WORDMASK;
}

// Returns true if chanNo is in the legal HSC channel range (1-5), else false. Does not
// check whether the channel is actually allocated -- see isAllocatedChanNo() for that.
static bool
isValidChanNo(int chanNo)
{
    return( (chanNo >= 1) && (chanNo <= HG_NUMCHANS) );
}

// Returns true if chanNo is in range AND this plugin currently holds an allocated
// HSCChannelP for it (i.e. a prior hga call succeeded and no hgf has freed it since).
static bool
isAllocatedChanNo(int chanNo)
{
    return( isValidChanNo(chanNo) && (handles[chanNo] != 0) );
}

// hgl -- "load scratch": copy 'count' words from PDP-1 core memory into channel chanNo's
// toBuf, the buffer HSC_MODE_TOMEM will later write from. This is plain core-to-buffer
// copying done directly in C, NOT an HSC operation -- it exists purely so a test program
// can stage known "expected write" data using ordinary dac instructions in its own memory,
// then have this command move it into the plugin's buffer ready for hgx.
// cbAddr is a flat (bank*4096+offset) core address of a 3-word control block:
//   word 0: count (0-4096)
//   word 1: bank (0-15)
//   word 2: starting address within that bank (0-4095)
// Address wraparound within the bank matches HSCexecute()'s own convention (wraps to 0 at
// the end of the 4096-word bank), so a test can set up wraparound source data consistently
// whether the wrap is exercised by this helper or by HSCexecute() itself.
// Returns HSC_OK (0) on success, HSC_ERR (-1) if chanNo/count/bank are out of range.
static int
loadScratch(PDP1 *pdp1P, int chanNo, int cbAddr)
{
int count, bank, addr, i;

    if( !isValidChanNo(chanNo) )
    {
        return( HSC_ERR );
    }

    count = (int)(pdp1P->core[cbAddr & (MAXMEM - 1)]);
    bank  = (int)(pdp1P->core[(cbAddr + 1) & (MAXMEM - 1)]);
    addr  = (int)(pdp1P->core[(cbAddr + 2) & (MAXMEM - 1)]);

    if( (count < 0) || (count > 4096) || (bank < 0) || (bank > 15) || (addr < 0) || (addr > 4095) )
    {
        iotCondLog(LOG_IOT, "hgl bad params count=%d bank=%d addr=%d\n", count, bank, addr);
        return( HSC_ERR );
    }

    for( i = 0; i < count; ++i )
    {
        toBuf[chanNo][i] = pdp1P->core[(bank * 4096) + addr];

        if( ++addr > 4095 )
        {
            addr = 0;       // same within-bank wraparound HSCexecute() itself uses
        }
    }

    return( HSC_OK );
}

// hgd -- "dump scratch": the mirror image of loadScratch(). Copies 'count' words from
// channel chanNo's fromBuf (the buffer HSC_MODE_FROMMEM most recently read into) out to
// PDP-1 core memory, so a test program can verify the read with ordinary lac/sad
// instructions. Same 3-word control block shape and wraparound behavior as loadScratch().
// Returns HSC_OK (0) on success, HSC_ERR (-1) if chanNo/count/bank are out of range.
static int
dumpScratch(PDP1 *pdp1P, int chanNo, int cbAddr)
{
int count, bank, addr, i;

    if( !isValidChanNo(chanNo) )
    {
        return( HSC_ERR );
    }

    count = (int)(pdp1P->core[cbAddr & (MAXMEM - 1)]);
    bank  = (int)(pdp1P->core[(cbAddr + 1) & (MAXMEM - 1)]);
    addr  = (int)(pdp1P->core[(cbAddr + 2) & (MAXMEM - 1)]);

    if( (count < 0) || (count > 4096) || (bank < 0) || (bank > 15) || (addr < 0) || (addr > 4095) )
    {
        iotCondLog(LOG_IOT, "hgd bad params count=%d bank=%d addr=%d\n", count, bank, addr);
        return( HSC_ERR );
    }

    for( i = 0; i < count; ++i )
    {
        pdp1P->core[(bank * 4096) + addr] = fromBuf[chanNo][i] & WORDMASK;

        if( ++addr > 4095 )
        {
            addr = 0;
        }
    }

    return( HSC_OK );
}

// Main entry point, called twice per IOT instruction executed (once per pulse edge) --
// see IOTs/iotHandler.h and the project-wide IOT completion convention in Claude/CLAUDE.md.
// Every command implemented here completes synchronously within a single call (no device
// ever needs the async completion/ioh pattern), so pulse edge and completion are otherwise
// ignored beyond the standard "only act on one edge" guard.
// Returns 1 if dev/ch was recognized and handled, 0 if dev was not ours (should not happen,
// dynamicIots.c only calls this for device 044) or ch did not match any known sub-command.
int
iotHandler(PDP1 *pdp1P, int dev, int pulse, int completion)
{
int ch, chanNo, cbAddr, result;
HSCRequest request;

    (void)completion;      // no command here uses the async completion/ioh pattern

    if( pulse )
    {
        return(1);          // only act on one edge, matches every other IOT in this codebase
    }

    if( dev != 044 )
    {
        return(0);           // not ours; should be unreachable, dynamicIots.c dispatches by device
    }

    ch = (MB(pdp1P) >> 6) & 077;
    chanNo = IO(pdp1P) & 077;      // every command below takes the channel number in IO bits 0-5
    cbAddr = AC(pdp1P) & (MAXMEM - 1);   // hgx/hgl/hgd take a control-block address in AC

    switch( ch )
    {
    case CMD_ALLOC:
        // hga -- allocate HSC channel 'chanNo' for this plugin's own use. IO out: HSC_OK on
        // success, HSC_ERR if chanNo is out of range or the channel is already assigned
        // (to this plugin, or to another device such as the drum on channel 1 or Type 340
        // Display on channel 3).
        if( !isValidChanNo(chanNo) || handles[chanNo] )
        {
            setIO(pdp1P, HSC_ERR);
            iotCondLog(LOG_IOT, "hga chan %d invalid or already allocated by us\n", chanNo);
            break;
        }

        handles[chanNo] = HSCallocateChannel(chanNo);
        setIO(pdp1P, (handles[chanNo]) ? HSC_OK : HSC_ERR);
        iotCondLog(LOG_IOT, "hga chan %d -> %d\n", chanNo, IO(pdp1P));
        break;

    case CMD_FREE:
        // hgf -- free a channel previously allocated via hga. IO out: HSC_OK on success,
        // HSC_ERR if chanNo is out of range or not currently allocated by this plugin.
        if( !isAllocatedChanNo(chanNo) )
        {
            setIO(pdp1P, HSC_ERR);
            break;
        }

        result = (HSCfreeChannel(handles[chanNo])) ? HSC_OK : HSC_ERR;
        setIO(pdp1P, result);
        if( result == HSC_OK )
        {
            handles[chanNo] = 0;
        }
        iotCondLog(LOG_IOT, "hgf chan %d -> %d\n", chanNo, IO(pdp1P));
        break;

    case CMD_EXEC:
        // hgx -- HSCexecute() on channel 'chanNo' using the 4-word control block at the
        // flat core address in AC: word0=mode (HSC_MODE_* bits, see hscgatewaydefs.ah),
        // word1=count, word2=bank, word3=address. IO out is HSCexecute()'s own raw return
        // value (HSC_OK/HSC_BUSY/HSC_ERR) so a test can assert against the exact documented
        // contract, or HSC_ERR if chanNo itself is not allocated by this plugin.
        //
        // Safety note for HSC_MODE_THREADED requests: HSCexecute() returns immediately here
        // (the data movement itself is synchronous inside HSCexecute()), but the count*5us
        // simulated delay is only actually enforced later, by hgw (HSCwait()). Keep THREADED
        // test counts small (a handful of words) -- hgw's call to HSCwait() blocks the whole
        // emulator's main thread for that count*5us, which is fine briefly but would be a
        // real, user-visible freeze for a large count.
        if( !isAllocatedChanNo(chanNo) )
        {
            setIO(pdp1P, HSC_ERR);
            break;
        }

        request.mode    = (int)(pdp1P->core[cbAddr]);
        request.count   = (int)(pdp1P->core[(cbAddr + 1) & (MAXMEM - 1)]);
        request.memBank = (int)(pdp1P->core[(cbAddr + 2) & (MAXMEM - 1)]);
        request.memAddr = (int)(pdp1P->core[(cbAddr + 3) & (MAXMEM - 1)]);
        request.fromBufferP = fromBuf[chanNo];
        request.toBufferP   = toBuf[chanNo];

        result = HSCexecute(handles[chanNo], &request);
        setIO(pdp1P, result);
        iotCondLog(LOG_IOT, "hgx chan %d mode %o count %d bank %d addr %o -> %d\n",
            chanNo, request.mode, request.count, request.memBank, request.memAddr, result);
        break;

    case CMD_STATUS:
        // hgs -- HSCgetStatus() on channel 'chanNo'. Never blocks; this is the primary way
        // test programs observe NORMAL-mode transfer progress (poll in a loop until the
        // result is HSC_DONE), exactly like every other device IOT in this codebase polls a
        // busy/ready flag rather than blocking inside an IOT call. IO out: the raw status
        // code, or HSC_ERR if chanNo is not allocated by this plugin.
        setIO(pdp1P, isAllocatedChanNo(chanNo) ? HSCgetStatus(handles[chanNo]) : HSC_ERR);
        break;

    case CMD_WAIT:
        // hgw -- HSCwait() on channel 'chanNo'. See the file header comment above: only
        // call this once hgs has already reported HSC_DONE for a NORMAL-mode request, or
        // for HSC_MODE_IMMEDIATE (always already done) / HSC_MODE_THREADED (bounded
        // count*5us busy-wait) requests -- calling it on a still-busy NORMAL-mode channel
        // would deadlock the emulator's main thread against itself. IO out: the raw status
        // code HSCwait() returns, or HSC_ERR if chanNo is not allocated by this plugin.
        setIO(pdp1P, isAllocatedChanNo(chanNo) ? HSCwait(handles[chanNo]) : HSC_ERR);
        break;

    case CMD_LOAD:
        // hgl -- load scratch buffer from core; see loadScratch() above for the control
        // block shape and semantics.
        setIO(pdp1P, loadScratch(pdp1P, chanNo, cbAddr));
        break;

    case CMD_DUMP:
        // hgd -- dump scratch buffer to core; see dumpScratch() above for the control block
        // shape and semantics.
        setIO(pdp1P, dumpScratch(pdp1P, chanNo, cbAddr));
        break;

    case CMD_RESET:
        // hgr -- simulate the front-panel stop/start/continue/examine/read-in switch
        // behavior documented in Docs/UsingHighSpeedChannels.md ("Behavior starting and
        // stopping"): calls HSCreset() directly, which marks every currently-assigned
        // channel HSC_ABORT regardless of which device (including this plugin, the drum,
        // or Type 340 Display) holds it. There is no way to trigger this from a real
        // front-panel switch inside an automated test, so this command exists purely to
        // make that path testable. IO out is always HSC_OK; HSCreset() has no failure mode.
        HSCreset();
        setIO(pdp1P, HSC_OK);
        iotCondLog(LOG_IOT, "hgr HSCreset() called\n");
        break;

    default:
        iotCondLog(LOG_IOT, "unrecognized ch %o for device 044\n", ch);
        return(0);
    }

    return(1);
}

// Called once when the emulator transitions to run state. Zeroes the channel-handle table
// so a prior run's (now stale, since HSCallocateChannel()'s malloc'd HSCChannelP is not
// guaranteed valid across an emulator restart) pointers are never reused. The scratch
// buffers don't need clearing -- every hgx/hgl/hgd caller supplies an explicit count, so
// leftover buffer content beyond that count is simply never read.
// No return value.
void
iotStart()
{
    iotCondLog(LOG_IOT, "IOT 44 (TestGateway) started\n");
    memset(handles, 0, sizeof(handles));
}

// Called once when the emulator transitions to halt state. No cleanup needed: this plugin
// holds no file descriptors or other OS resources, and deliberately does not free its HSC
// channels here -- a test program that halted mid-test (e.g. to inspect a failure on the
// console) should still see its channels allocated if it continues.
// No return value.
void
iotStop()
{
    iotCondLog(LOG_IOT, "IOT 44 (TestGateway) stopped\n");
}
