/*
 * The am1 optimizer's control-flow graph: basic blocks that can only be
 * entered at their first word, edges to the blocks control can reach, one
 * shared UNKNOWN sink for every target the analysis cannot name, and the
 * reachability walk from the program's entry points.  Also the -O=flow dump
 * and the report's per-bank flow section.
 *
 * Works on the words optrefs.c classified as code, reading the parse tree and
 * symbol tables without modifying them; everything derived lives in the
 * OptTable of optimizer.h.  Called single threaded from optimize() in
 * optimizer.c; allocation failure is fatal, as elsewhere in am1.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>

#include "am1.h"
#include "y.tab.h"
#include "optimizer.h"

// Per-bank "a block starts at this address" maps, one byte per address.  All
// are complete before any block is formed, because a via-pointer jump in one
// bank can name a target in a bank already walked.
typedef struct
{
    char *marksP[MAXBANK + 1];      // NILP for a bank that emitted nothing
} StartMap, *StartMapP;

// The targets the stores into a jmp or jsp leave there, when a dac or dio
// stores the word whole.  A program needing more than this many distinct
// targets in one word is not recovered: the edges stop and the sink stays.
#define STORED_MAXTARGETS   16

// How far back a store's value is looked for, in words.
#define VALUE_SCAN_LIMIT    64

typedef struct
{
    int count;                              // distinct targets found
    int addrs[STORED_MAXTARGETS];           // in the transfer word's own bank
} StoredTargets, *StoredTargetsP;

// The word flags the control-flow dump prints, in the order it prints them.
static const struct
{
    unsigned int bit;
    const char *nameP;
} flowFlagNames[] =
{
    { OPTF_ENTRY,       "entry" },
    { OPTF_BLOCKSTART,  "blockstart" },
    { OPTF_HASLABEL,    "label" },
    { OPTF_AFTERSKIP,   "afterskip" },
    { OPTF_UNREACHED,   "unreached" },
    { OPTF_RESUMED,     "resumed" },
    { OPTF_XCTONLY,     "xctonly" }
};

static int hasExportedLabel(OptWordP entryP);
static OptEntryCause entryCause(OptTableP tableP, OptWordP entryP);
static void markFlowWordFlags(OptTableP tableP);
static void countExcludedWords(OptTableP tableP);
static void allocStartMap(OptTableP tableP, StartMapP mapP);
static void freeStartMap(OptTableP tableP, StartMapP mapP);
static void markStart(StartMapP mapP, int bank, int addr);
static void markBlockStarts(OptTableP tableP, StartMapP mapP);
static void markControlTargets(OptTableP tableP, StartMapP mapP, OptWordP entryP);
static int leavesRegister(OptWordP wP, int wantIO);
static int fallOnly(OptTableP tableP, OptWordP wP);
static int constantLoaded(OptTableP tableP, OptWordP loadP, int *valueP);
static int valueBefore(OptTableP tableP, OptWordP siteP, int wantIO, int *valueP);
static int storedTargets(OptTableP tableP, OptWordP entryP, StoredTargetsP targetsP);
static void formBlocks(OptTableP tableP, StartMapP mapP);
static OptBlockP newBlock(OptTableP tableP, OptWordP entryP);
static void setBlockEndKinds(OptTableP tableP);
static OptFlowEdgeP addFlowEdge(OptTableP tableP, OptBlockP blockP, OptFlowKind kind);
static void addSinkEdge(OptTableP tableP, OptBlockP blockP, OptFlowKind kind, OptSink sink);
static void addTargetEdge(OptTableP tableP, OptBlockP blockP, OptFlowKind kind, int bank, int addr);
static void markRecovered(OptFlowEdgeP edgeP, OptRecovery recovered);
static int viaJumpEdges(OptTableP tableP, OptBlockP blockP, OptFlowKind kind, OptWordP entryP);
static void addTransferEdges(OptTableP tableP, OptBlockP blockP, OptFlowKind kind, OptWordP entryP);
static void buildBlockEdges(OptTableP tableP, OptBlockP blockP);
static void markEntries(OptTableP tableP);
static void walkReachability(OptTableP tableP);
static void walkResumption(OptTableP tableP);
static void markUnreached(OptTableP tableP);
static int blockHasUnreachedWord(OptTableP tableP, OptBlockP blockP);
static void dumpReturnSites(FILE *fP, OptTableP tableP);

// Overlay the control-flow graph on a decoded, referenced table.  The start
// maps are complete before any block is formed, so every in-graph edge lands
// on the first word of a block; edges need the blocks, reachability the edges.
void
optBuildFlow(OptTableP tableP)
{
StartMap map;
OptBlockP blockP;

    if( !tableP )
    {
        return;
    }

    markFlowWordFlags(tableP);
    countExcludedWords(tableP);

    // Must run here: a callee that steps past inline arguments returns further
    // on, and that address must be marked a block start with the other edge
    // targets.  It reads OPTF_AFTERSKIP, so it follows markFlowWordFlags().
    optResolveReturns(tableP);

    allocStartMap(tableP, &map);
    markBlockStarts(tableP, &map);
    formBlocks(tableP, &map);
    freeStartMap(tableP, &map);

    setBlockEndKinds(tableP);

    for( blockP = tableP->blocksP; blockP; blockP = blockP->nextP )
    {
        buildBlockEdges(tableP, blockP);

        tableP->graphWords += blockP->wordCount;

        if( blockP->wordCount > tableP->largestBlock )
        {
            tableP->largestBlock = blockP->wordCount;
        }
    }

    markEntries(tableP);
    walkReachability(tableP);

    // After the reachability walk, because it only ever considers the blocks
    // that walk did not reach, and before markUnreached(), which records what
    // it found on the words.
    walkResumption(tableP);
    markUnreached(tableP);
}

// Is a word part of the control-flow graph?  A reserved word has no value, an
// overlaid address has no single word the machine would fetch, and a word
// optrefs.c did not call code is not believed to execute.
// Returns 1 when the word is in the graph, 0 when it is not or is NILP.
int
inGraph(OptWordP entryP)
{
    if( !entryP )
    {
        return(0);
    }

    if( entryP->flags & (OPTF_RESERVED | OPTF_DUPADDR) )
    {
        return(0);
    }

    if( !(entryP->flags & OPTF_CODE) )
    {
        return(0);
    }

    return(1);
}

// Find the first word emitted at a bank and address.
// Returns the word, or NILP when the bank emitted nothing, nothing was
// emitted at that address, or the coordinates are out of range.
OptWordP
wordAt(OptTableP tableP, int bank, int addr)
{
OptBankP bankP;

    if( (bank < 0) || (bank > MAXBANK) || (addr < 0) || (addr >= BANKSIZE) )
    {
        return(NILP);
    }

    if( !(bankP = tableP->banksP[bank]) )
    {
        return(NILP);
    }

    return( bankP->wordsP[addr] );
}

// Is a word skip-class, one that may leave the next word unexecuted?  The
// skip group (codes 64 and 65) plus isp, sad, sas and div.  div skips UNLESS it
// overflows, the reverse of the others, so T2 refuses to invert it; it still
// ends a block the same way.  Code 56 is also dis (divide step, no skip) on a
// machine without multiply/divide hardware; counting it a skip only makes P4
// refuse a rewrite, the safe direction.
//
// An in-out transfer skips only where its device says so, and the Type 340's
// four flag tests are the only ones documented to (dsp, dss, dsv and dsh,
// device 17, sub-bits 1, 2, 4 and 10 in any combination; see
// Docs/UsingType340Display.md).
// Returns 1 for a skip-class word, 0 otherwise or for NILP.
int
optSkipWord(OptWordP entryP)
{
    if( !entryP || (entryP->flags & OPTF_RESERVED) )
    {
        return(0);
    }

    if( entryP->decode.group == OPTG_SKIP )
    {
        return(1);
    }

    if( (entryP->decode.group == OPTG_IOT) && (entryP->decode.iotDevice == 017) &&
        (entryP->decode.iotSub & 017) )
    {
        return(1);
    }

    if( entryP->decode.group == OPTG_MEMREF )
    {
        switch( entryP->decode.opcode )
        {
        case 046:       // isp
        case 050:       // sad
        case 052:       // sas
        case 056:       // div, which skips UNLESS it overflows; dis shares it
            return(1);

        default:
            return(0);
        }
    }

    return(0);
}

// How does a word end a basic block of its own accord?  xct is not a
// terminator though the word it executes may jump; that can only call a word
// reached that is not, the safe direction.  optreturn.c cuts its callee walks
// with this, exactly as the graph does.
// Returns the ending kind, or OPTBE_COUNT when the word is not a terminator.
OptBlockEnd
optTerminator(OptWordP entryP)
{
    if( !entryP || (entryP->flags & OPTF_RESERVED) )
    {
        return(OPTBE_COUNT);
    }

    if( optSkipWord(entryP) )
    {
        return(OPTBE_SKIP);
    }

    if( entryP->decode.group == OPTG_MEMREF )
    {
        switch( entryP->decode.opcode )
        {
        case 060:       // jmp
            return(OPTBE_JUMP);

        case 062:       // jsp
        case 016:       // cal, and jda when bit 5 is set
            return(OPTBE_CALL);

        default:
            return(OPTBE_COUNT);
        }
    }

    // hlt stops the machine at TP9.  Nothing else in the operate group ends a
    // block; the rest are register operations the next word follows.
    if( (entryP->decode.group == OPTG_OPERATE) && (entryP->decode.microBits & OPTM_HLT) )
    {
        return(OPTBE_HALT);
    }

    return(OPTBE_COUNT);
}

// Does any label on a word export it, so another assembly may jump to it?
// Returns 1 when at least one label is exported, 0 otherwise.
static int
hasExportedLabel(OptWordP entryP)
{
OptLabelP labelP;

    for( labelP = entryP->labelsP; labelP; labelP = labelP->nextP )
    {
        if( labelP->symP && (labelP->symP->flags & SYMF_EXPORTED) )
        {
            return(1);
        }
    }

    return(0);
}

// How many sequence-break frames does the program leave room for?  Each Type
// 20 channel owns four words of bank 0 from address 0, so sixteen channels use
// 0-077 and programs conventionally begin at 0100; with channel 0 alone they
// begin at 4.  The frames are those wholly below a bank-0 start address; a
// program ending in 'stop' or starting in another bank keeps all sixteen.  A
// program with no esm, asc or isb has none, whatever its start address.
// Returns the number of channels, 0 to OPTSBS_CHANNELS.
int
optSbsChannels(OptTableP tableP)
{
int addr;

    if( !optSbsEnableCount(tableP) )
    {
        return(0);
    }

    if( !tableP->hasStart || (((tableP->startAddr >> 12) & 017) != 0) )
    {
        return(OPTSBS_CHANNELS);
    }

    addr = (tableP->startAddr & ADDRMASK);

    if( addr >= (OPTSBS_CHANNELS * OPTSBS_FRAMESIZE) )
    {
        return(OPTSBS_CHANNELS);
    }

    return(addr / OPTSBS_FRAMESIZE);
}

// Is a word a sequence-break enable command: esm, asc or isb?  (isb runs a
// handler even with the system off; lsm, dsc and cbs are not enables.)  The
// device code alone decides, since asc and isb carry a channel in the sub
// field and the wait bits may be set; a data word that decodes as one counts
// too.  Over-counting only keeps frames, which only withholds rewrites.
// Returns the mnemonic, or NILP when the word is not an enable or is NILP.
const char *
optSbsEnableName(OptWordP entryP)
{
    if( !entryP || (entryP->flags & OPTF_RESERVED) )
    {
        return(NILP);
    }

    if( entryP->decode.group != OPTG_IOT )
    {
        return(NILP);
    }

    switch( entryP->decode.iotDevice )
    {
    case OPTSBS_ESM_DEVICE:
        return("esm");

    case OPTSBS_ASC_DEVICE:
        return("asc");

    case OPTSBS_ISB_DEVICE:
        return("isb");

    default:
        return(NILP);
    }
}

// Count the enable commands among all emitted words and cache the answer on
// the table.  Caching is safe: -O1 rewrites memory reference words only, so it
// can neither make nor unmake an enable.
static void
countSbsEnables(OptTableP tableP)
{
int i;
OptWordP entryP;

    tableP->sbsEnableCount = 0;
    tableP->sbsFirstEnableP = NILP;

    for( i = 0; i < tableP->count; ++i )
    {
        entryP = tableP->entriesPP[i];

        if( optSbsEnableName(entryP) )
        {
            if( !tableP->sbsFirstEnableP )
            {
                tableP->sbsFirstEnableP = entryP;
            }

            ++tableP->sbsEnableCount;
        }
    }

    tableP->sbsEnablesCounted = 1;
}

// Count the emitted words that are enable commands.
// Returns the count, 0 when there are none.
int
optSbsEnableCount(OptTableP tableP)
{
    if( !tableP->sbsEnablesCounted )
    {
        countSbsEnables(tableP);
    }

    return(tableP->sbsEnableCount);
}

// The first enable command in emission order.
// Returns the word, or NILP when the program holds none.
OptWordP
optSbsFirstEnable(OptTableP tableP)
{
    if( !tableP->sbsEnablesCounted )
    {
        countSbsEnables(tableP);
    }

    return(tableP->sbsFirstEnableP);
}

// Print the enable commands the frame count rests on, as one line shared by
// the flow and call-graph dumps: "sbs enables: none", or a comma-separated
// list of BBAAAA MNEMONIC VALUE.
void
optDumpSbsEnables(FILE *fP, OptTableP tableP)
{
int i;
int any;
OptWordP entryP;
const char *nameP;

    fprintf(fP, "sbs enables:");

    if( !optSbsEnableCount(tableP) )
    {
        fprintf(fP, " none\n");
        return;
    }

    any = 0;

    for( i = 0; i < tableP->count; ++i )
    {
        entryP = tableP->entriesPP[i];

        if( (nameP = optSbsEnableName(entryP)) )
        {
            fprintf(fP, "%s %02o%04o %s %06o", (any)?",":"", entryP->bank, entryP->addr,
                nameP, entryP->value);
            any = 1;
        }
    }

    fprintf(fP, "\n");
}

// Is an address a sequence-break handler entry?  The hardware saves AC, PC and
// IO at 4n+0..2 and starts executing at 4n+3, so 4n+3 is entered without the
// program ever naming it.  Only the frames optSbsChannels() allows count.
// Returns 1 for a handler entry address, 0 otherwise.
int
optIsSbsEntry(OptTableP tableP, int bank, int addr)
{
    if( bank != 0 )
    {
        return(0);
    }

    if( (addr < 0) || (addr >= (optSbsChannels(tableP) * OPTSBS_FRAMESIZE)) )
    {
        return(0);
    }

    return( (addr % OPTSBS_FRAMESIZE) == OPTSBS_ENTRYSLOT );
}

// Why is a word an entry of the reachability walk?  The first cause that
// applies is reported, so the per-cause counts sum to the entry count.
// Returns the cause, or OPTEN_COUNT when the word is not an entry.
static OptEntryCause
entryCause(OptTableP tableP, OptWordP entryP)
{
    if( entryP->flags & OPTF_START )
    {
        return(OPTEN_START);
    }

    // A word whose address is used as a value may be reached by an indirect
    // jump or an xct this analysis cannot follow.
    if( entryP->flags & OPTF_TAKEN )
    {
        return(OPTEN_TAKEN);
    }

    if( hasExportedLabel(entryP) )
    {
        return(OPTEN_EXPORT);
    }

    if( optIsSbsEntry(tableP, entryP->bank, entryP->addr) )
    {
        return(OPTEN_SBS);
    }

    return(OPTEN_COUNT);
}

// Clear old flow state and set the per-word flags that do not depend on the
// graph.
static void
markFlowWordFlags(OptTableP tableP)
{
int i;
OptWordP entryP;
OptWordP prevP;

    for( i = 0; i < tableP->count; ++i )
    {
        entryP = tableP->entriesPP[i];
        entryP->flags &= ~OPTF_FLOW_MASK;
        entryP->blockP = NILP;

        // P1 treats ANY label as a side entry, even a data label on an
        // interior word, so this is not the same thing as a block start.
        if( entryP->labelsP )
        {
            entryP->flags |= OPTF_HASLABEL;
        }

        // For P4: might this word be skipped?  A word below that decodes as a
        // skip counts whether code or data, at any overlay; being wrong this
        // way only suppresses a transform.  Address 0 does not wrap.
        if( entryP->addr > 0 )
        {
            for( prevP = wordAt(tableP, entryP->bank, (entryP->addr - 1)); prevP; prevP = prevP->sameAddrP )
            {
                if( optSkipWord(prevP) )
                {
                    entryP->flags |= OPTF_AFTERSKIP;
                    break;
                }
            }
        }
    }
}

// Count the words the graph leaves out, by reason, so the report can account
// for them.  Reserved words are counted by the table already.
static void
countExcludedWords(OptTableP tableP)
{
int i;
OptWordP entryP;

    for( i = 0; i < tableP->count; ++i )
    {
        entryP = tableP->entriesPP[i];

        if( inGraph(entryP) || (entryP->flags & OPTF_RESERVED) )
        {
            continue;
        }

        if( entryP->flags & OPTF_DUPADDR )
        {
            ++tableP->overlaidWords;
        }
        else
        {
            ++tableP->notCodeWords;
        }
    }
}

// Allocate a start map for every bank that emitted a word.
static void
allocStartMap(OptTableP tableP, StartMapP mapP)
{
int bank;

    for( bank = 0; bank <= MAXBANK; ++bank )
    {
        mapP->marksP[bank] = NILP;

        if( !tableP->banksP[bank] )
        {
            continue;
        }

        if( !(mapP->marksP[bank] = (char *)calloc(BANKSIZE, sizeof(char))) )
        {
            fprintf(stderr, "am1: out of memory building the optimizer control-flow graph\n");
            exit(1);
        }
    }
}

// Release the start maps.
static void
freeStartMap(OptTableP tableP, StartMapP mapP)
{
int bank;

    (void)tableP;

    for( bank = 0; bank <= MAXBANK; ++bank )
    {
        if( mapP->marksP[bank] )
        {
            free(mapP->marksP[bank]);
            mapP->marksP[bank] = NILP;
        }
    }
}

// Mark one address as starting a block.  Coordinates outside the map, or in a
// bank that emitted nothing, are ignored: there is no word there to begin a
// block with, and the edge that named it will find the sink on its own.
static void
markStart(StartMapP mapP, int bank, int addr)
{
    if( (bank < 0) || (bank > MAXBANK) || (addr < 0) || (addr >= BANKSIZE) )
    {
        return;
    }

    if( !mapP->marksP[bank] )
    {
        return;
    }

    mapP->marksP[bank][addr] = 1;
}

// Mark every address that begins a block: the entry words and every
// control-transfer target.
static void
markBlockStarts(OptTableP tableP, StartMapP mapP)
{
int i;
OptWordP entryP;

    for( i = 0; i < tableP->count; ++i )
    {
        entryP = tableP->entriesPP[i];

        if( !inGraph(entryP) )
        {
            continue;
        }

        // The start address, jump targets, taken addresses, exported labels
        // and sequence-break entries can all be entered from outside this
        // analysis' view, so each must begin a block.
        if( entryP->flags & (OPTF_START | OPTF_JUMPTARGET | OPTF_TAKEN) )
        {
            markStart(mapP, entryP->bank, entryP->addr);
        }
        else if( hasExportedLabel(entryP) || optIsSbsEntry(tableP, entryP->bank, entryP->addr) )
        {
            markStart(mapP, entryP->bank, entryP->addr);
        }

        markControlTargets(tableP, mapP, entryP);
    }
}

// Mark the addresses one word's control transfers name.  They come from the
// DECODED bits, as in buildBlockEdges(), not from optrefs.c's edges, which
// exist only where the source spelled a memory reference.
static void
markControlTargets(OptTableP tableP, StartMapP mapP, OptWordP entryP)
{
OptBlockEnd end;
OptEdgeP edgeP;
int addr;

    end = optTerminator(entryP);
    addr = entryP->decode.address;

    if( end == OPTBE_COUNT )
    {
        return;
    }

    // Every terminator's own next word begins a block: the fall-through of a
    // skip, the return of a call, the resume of a halt, and, after a jmp, a
    // word nothing falls into but which cannot belong to this block either.
    markStart(mapP, entryP->bank, ((entryP->addr + 1) & ADDRMASK));

    // A callee proved to step past inline arguments returns further on.  The
    // word after the call, marked above, is the argument: data, on which
    // formBlocks() begins no block.
    if( (end == OPTBE_CALL) && (entryP->returnCase == OPTRC_STEPPED) )
    {
        markStart(mapP, entryP->bank, entryP->returnAddr);
    }

    if( end == OPTBE_SKIP )
    {
        markStart(mapP, entryP->bank, ((entryP->addr + 2) & ADDRMASK));
        return;
    }

    if( (end != OPTBE_JUMP) && (end != OPTBE_CALL) )
    {
        return;
    }

    // A patched word's address field is a run-time value and DEBREAK returns
    // to whatever it interrupted.  Both reach the sink and mark nothing.
    if( entryP->flags & OPTF_PATCHED )
    {
        return;
    }

    if( entryP->decode.memIndirect )
    {
        if( (entryP->bank == 0) && (addr == OPTDEBREAK_ADDR) )
        {
            return;
        }

        // optrefs.c followed the pointer: its via-pointer jump edges are
        // where control lands.
        for( edgeP = entryP->outP; edgeP; edgeP = edgeP->nextOutP )
        {
            if( (edgeP->role == OPTR_JUMP) && (edgeP->flags & OPTEF_VIAPOINTER) )
            {
                markStart(mapP, edgeP->toBank, edgeP->toAddr);
            }
        }

        return;
    }

    if( entryP->decode.opcode == 016 )
    {
        // jda deposits AC in the word it names and enters the word after it;
        // cal is jda 0100, so it ignores its address field and enters 0101 of
        // its own bank.
        if( entryP->decode.indirect )
        {
            markStart(mapP, entryP->bank, ((addr + 1) & ADDRMASK));
        }
        else
        {
            markStart(mapP, entryP->bank, CAL_JUMP_ADDR);
        }

        return;
    }

    markStart(mapP, entryP->bank, addr);

    if( entryP->flags & OPTF_WRITTEN )
    {
        StoredTargets stored;
        int i;

        (void)storedTargets(tableP, entryP, &stored);

        for( i = 0; i < stored.count; ++i )
        {
            markStart(mapP, entryP->bank, stored.addrs[i]);
        }
    }
}

// Does a word leave AC, or IO, as it found it?  Anything not known to is
// taken to change it, which only loses a target, the safe direction.
// Returns 1 when the register is left alone, 0 otherwise.
static int
leavesRegister(OptWordP wP, int wantIO)
{
    if( wP->decode.group == OPTG_SKIP )
    {
        return(1);
    }

    if( wP->decode.group != OPTG_MEMREF )
    {
        return(0);
    }

    switch( wP->decode.opcode )
    {
    case 024:       // dac
    case 026:       // dap
    case 030:       // dip
    case 032:       // dio
    case 034:       // dzm
    case 050:       // sad
    case 052:       // sas
        return(1);

    case 022:       // lio
        return(!wantIO);

    case 002:       // and
    case 004:       // ior
    case 006:       // xor
    case 020:       // lac
    case 040:       // add
    case 042:       // sub
    case 044:       // idx
    case 046:       // isp
        return(wantIO);

    default:
        return(0);
    }
}

// Can control reach a word only by falling in from the word below it?  Not
// if anything names it or may enter it from outside, and not if a skip two
// words back can land on it.  A call that steps past inline arguments returns
// past data words, which are not in the graph, so the caller stops there.
// Returns 1 when the word below is the only way in, 0 otherwise.
static int
fallOnly(OptTableP tableP, OptWordP wP)
{
OptWordP twoBackP;

    if( wP->flags & (OPTF_START | OPTF_JUMPTARGET | OPTF_TAKEN) )
    {
        return(0);
    }

    if( hasExportedLabel(wP) || optIsSbsEntry(tableP, wP->bank, wP->addr) )
    {
        return(0);
    }

    if( (wP->addr >= 2) && (twoBackP = wordAt(tableP, wP->bank, (wP->addr - 2))) &&
        inGraph(twoBackP) && optSkipWord(twoBackP) )
    {
        return(0);
    }

    return(1);
}

// The value a direct lac or lio loads, when the word it names is one nothing
// writes.
// Returns 1 with *valueP set, 0 when the word may hold anything.
static int
constantLoaded(OptTableP tableP, OptWordP loadP, int *valueP)
{
OptEdgeP edgeP;
OptWordP srcP;

    if( loadP->decode.memIndirect )
    {
        return(0);
    }

    srcP = NILP;

    for( edgeP = loadP->outP; edgeP; edgeP = edgeP->nextOutP )
    {
        if( (edgeP->role == OPTR_READ) && edgeP->toP && !(edgeP->flags & OPTEF_INDIRECT) )
        {
            srcP = edgeP->toP;
            break;
        }
    }

    if( !srcP )
    {
        srcP = wordAt(tableP, loadP->bank, loadP->decode.address);
    }

    if( !srcP || (srcP->flags & (OPTF_WRITTEN | OPTF_PATCHED | OPTF_MAYBE_WRITTEN |
                                 OPTF_RESERVED | OPTF_DUPADDR)) )
    {
        return(0);
    }

    *valueP = srcP->value;
    return(1);
}

// What AC (or IO, when wantIO) holds as a store runs, read back through the
// straight-line code in front of it: a law, or a load of a word nothing
// writes, with nothing between that changes the register.
// Returns 1 with *valueP set, 0 when the code does not say.
static int
valueBefore(OptTableP tableP, OptWordP siteP, int wantIO, int *valueP)
{
OptWordP wP;
OptWordP prevP;
OptBlockEnd end;
int steps;

    wP = siteP;

    for( steps = 0; steps < VALUE_SCAN_LIMIT; ++steps )
    {
        if( wP != siteP )
        {
            if( !wantIO && (wP->decode.group == OPTG_LAW) )
            {
                *valueP = (wP->decode.indirect)?((~wP->decode.address) & WRDMASK):wP->decode.address;
                return(1);
            }

            if( (wP->decode.group == OPTG_MEMREF) && (wP->decode.opcode == ((wantIO)?022:020)) )
            {
                return(constantLoaded(tableP, wP, valueP));
            }

            if( !leavesRegister(wP, wantIO) )
            {
                return(0);
            }
        }

        if( !fallOnly(tableP, wP) || (wP->addr == 0) )
        {
            return(0);
        }

        prevP = wordAt(tableP, wP->bank, (wP->addr - 1));

        if( !inGraph(prevP) )
        {
            return(0);
        }

        // A skip falls into the word after it; any other terminator does not.
        end = optTerminator(prevP);

        if( (end != OPTBE_COUNT) && (end != OPTBE_SKIP) )
        {
            return(0);
        }

        wP = prevP;
    }

    return(0);
}

// Where a jmp or jsp can go once a dac or dio has stored it whole: the target
// of each stored word, when the code in front of every store states it and it
// is the same instruction, direct.  A dzm, an idx, a store through a pointer,
// a write through an unresolved pointer, or a stored word that is anything
// else makes the target a run-time fact.  The assembled target is not
// collected: it is where the word goes before any store runs, and the caller
// has its edge already.
// Returns 1 when every store is known, 0 when any is not; targetsP holds the
// known targets either way.
static int
storedTargets(OptTableP tableP, OptWordP entryP, StoredTargetsP targetsP)
{
OptEdgeP inP;
OptWordP siteP;
int every;
int known;
int value;
int addr;
int i;

    targetsP->count = 0;
    value = 0;
    every = !(entryP->flags & OPTF_MAYBE_WRITTEN);

    for( inP = entryP->inP; inP; inP = inP->nextInP )
    {
        if( (inP->role != OPTR_WRITE) || !(siteP = inP->fromP) || !inGraph(siteP) )
        {
            continue;
        }

        // A store whose own address field is patched writes wherever the
        // patch says; the edge here is only its assembled address.
        if( (inP->flags & OPTEF_PLACEHOLDER) || (siteP->decode.group != OPTG_MEMREF) ||
            siteP->decode.memIndirect )
        {
            every = 0;
            continue;
        }

        switch( siteP->decode.opcode )
        {
        case 024:       // dac
            known = valueBefore(tableP, siteP, 0, &value);
            break;

        case 032:       // dio
            known = valueBefore(tableP, siteP, 1, &value);
            break;

        default:
            known = 0;
            break;
        }

        if( !known || (((value >> 12) & 077) != entryP->decode.opcode) )
        {
            every = 0;
            continue;
        }

        addr = value & ADDRMASK;

        if( addr == entryP->decode.address )
        {
            continue;
        }

        for( i = 0; (i < targetsP->count) && (targetsP->addrs[i] != addr); ++i )
        {
        }

        if( i < targetsP->count )
        {
            continue;
        }

        if( targetsP->count == STORED_MAXTARGETS )
        {
            every = 0;
            continue;
        }

        targetsP->addrs[targetsP->count++] = addr;
    }

    return(every);
}

// Cut the in-graph words into blocks, walking each bank in address order, so
// a block simply stops where the next address begins one.
static void
formBlocks(OptTableP tableP, StartMapP mapP)
{
int bank;
int addr;
OptWordP entryP;
OptWordP prevP;
OptBlockP blockP;

    for( bank = 0; bank <= MAXBANK; ++bank )
    {
        if( !mapP->marksP[bank] )
        {
            continue;
        }

        blockP = NILP;
        prevP = NILP;

        for( addr = 0; addr < BANKSIZE; ++addr )
        {
            entryP = wordAt(tableP, bank, addr);

            if( !inGraph(entryP) )
            {
                // A gap, a data word or an overlaid address: whatever comes
                // after it cannot continue the block that was open.
                blockP = NILP;
                prevP = NILP;
                continue;
            }

            // A block begins here when nothing is open, when the maps say an
            // address is entered from somewhere, or when the previous word
            // ended a block of its own accord.
            if( !blockP || mapP->marksP[bank][addr] || (optTerminator(prevP) != OPTBE_COUNT) )
            {
                blockP = newBlock(tableP, entryP);
            }
            else
            {
                blockP->endAddr = addr;
                blockP->lastP = entryP;
                ++blockP->wordCount;
            }

            entryP->blockP = blockP;
            prevP = entryP;
        }
    }
}

// Start a new one-word block at a word and append it to the table's list.
// Blocks are numbered from 1 in the order they are created, which is bank
// then address order, so the id ordering and the address ordering agree.
// Returns the new block; allocation failure is fatal.
static OptBlockP
newBlock(OptTableP tableP, OptWordP entryP)
{
OptBlockP blockP;

    if( !(blockP = (OptBlockP)calloc(1, sizeof(OptBlock))) )
    {
        fprintf(stderr, "am1: out of memory building the optimizer control-flow graph\n");
        exit(1);
    }

    blockP->id = ++tableP->blockCount;
    blockP->bank = entryP->bank;
    blockP->startAddr = entryP->addr;
    blockP->endAddr = entryP->addr;
    blockP->wordCount = 1;
    blockP->firstP = entryP;
    blockP->lastP = entryP;
    blockP->endKind = OPTBE_GAP;

    entryP->flags |= OPTF_BLOCKSTART;

    if( tableP->blocksTailP )
    {
        tableP->blocksTailP->nextP = blockP;
    }
    else
    {
        tableP->blocksP = blockP;
    }

    tableP->blocksTailP = blockP;
    return(blockP);
}

// Decide how each block ends: by its terminator, or else by what it ran into,
// which tells whether it flows on into the next block.
static void
setBlockEndKinds(OptTableP tableP)
{
OptBlockP blockP;
OptBlockEnd end;
OptWordP nextP;

    for( blockP = tableP->blocksP; blockP; blockP = blockP->nextP )
    {
        if( (end = optTerminator(blockP->lastP)) != OPTBE_COUNT )
        {
            blockP->endKind = end;
            continue;
        }

        if( blockP->endAddr >= (BANKSIZE - 1) )
        {
            blockP->endKind = OPTBE_GAP;
            continue;
        }

        nextP = wordAt(tableP, blockP->bank, (blockP->endAddr + 1));

        if( !nextP )
        {
            blockP->endKind = OPTBE_GAP;
        }
        else if( !inGraph(nextP) )
        {
            blockP->endKind = OPTBE_NOTCODE;
        }
        else
        {
            blockP->endKind = OPTBE_BOUNDARY;
        }
    }
}

// Append a successor edge to a block and count it on the table.
// Returns the new edge, with its target still unset; allocation failure is
// fatal.
static OptFlowEdgeP
addFlowEdge(OptTableP tableP, OptBlockP blockP, OptFlowKind kind)
{
OptFlowEdgeP edgeP;

    if( !(edgeP = (OptFlowEdgeP)calloc(1, sizeof(OptFlowEdge))) )
    {
        fprintf(stderr, "am1: out of memory building the optimizer control-flow graph\n");
        exit(1);
    }

    edgeP->kind = kind;
    edgeP->sink = OPTSK_NONE;

    if( blockP->succTailP )
    {
        blockP->succTailP->nextP = edgeP;
    }
    else
    {
        blockP->succP = edgeP;
    }

    blockP->succTailP = edgeP;
    ++blockP->succCount;
    ++tableP->flowEdgeCount;
    ++tableP->flowKindCounts[kind];

    return(edgeP);
}

// Append an edge to the shared UNKNOWN sink, with the reason the analysis
// lost the thread.  A sink edge carries no reachability: it says the target
// is unknown, not that control stopped.
static void
addSinkEdge(OptTableP tableP, OptBlockP blockP, OptFlowKind kind, OptSink sink)
{
OptFlowEdgeP edgeP;

    edgeP = addFlowEdge(tableP, blockP, kind);
    edgeP->sink = sink;
    ++tableP->sinkEdgeCount;
    ++tableP->sinkCounts[sink];
}

// Append an edge to whatever lies at a bank and address, which is a block
// when the graph holds a word there and the sink when it does not.  Every
// in-graph target reached here is the first word of its block, because
// markBlockStarts() marked it before any block was formed.
static void
addTargetEdge(OptTableP tableP, OptBlockP blockP, OptFlowKind kind, int bank, int addr)
{
OptWordP targetP;
OptFlowEdgeP edgeP;

    if( !(targetP = wordAt(tableP, bank, addr)) )
    {
        addSinkEdge(tableP, blockP, kind, OPTSK_NOWORD);
        return;
    }

    if( targetP->flags & OPTF_DUPADDR )
    {
        addSinkEdge(tableP, blockP, kind, OPTSK_OVERLAID);
        return;
    }

    if( !inGraph(targetP) || !targetP->blockP )
    {
        addSinkEdge(tableP, blockP, kind, OPTSK_NOTCODE);
        return;
    }

    edgeP = addFlowEdge(tableP, blockP, kind);
    edgeP->toP = targetP->blockP;
    edgeP->toBank = bank;
    edgeP->toAddr = addr;
    edgeP->crossbank = (bank != blockP->bank);
    ++targetP->blockP->predCount;
}

// Name an edge as one the graph once lacked, when it reaches a block.  One that
// lands outside the graph stays a plain sink, counted under its own reason.
static void
markRecovered(OptFlowEdgeP edgeP, OptRecovery recovered)
{
    if( edgeP->toP )
    {
        edgeP->recovered = recovered;
    }
}

// Make one edge per via-pointer jump optrefs.c derived for an indirect
// transfer; the edge's crossbank mark comes from the banks themselves.
// Returns how many edges were made, 0 when the pointer could not be followed.
static int
viaJumpEdges(OptTableP tableP, OptBlockP blockP, OptFlowKind kind, OptWordP entryP)
{
OptEdgeP edgeP;
int made;

    made = 0;

    for( edgeP = entryP->outP; edgeP; edgeP = edgeP->nextOutP )
    {
        if( (edgeP->role != OPTR_JUMP) || !(edgeP->flags & OPTEF_VIAPOINTER) )
        {
            continue;
        }

        addTargetEdge(tableP, blockP, kind, edgeP->toBank, edgeP->toAddr);
        ++made;
    }

    return(made);
}

// Make the edges for a jmp or a jsp: the rules are the same and only the edge
// kind differs.
static void
addTransferEdges(OptTableP tableP, OptBlockP blockP, OptFlowKind kind, OptWordP entryP)
{
    // A dap or dip writes this word's address field, so where it goes is a
    // run-time fact and no static target is the right one.
    if( entryP->flags & OPTF_PATCHED )
    {
        addSinkEdge(tableP, blockP, kind, OPTSK_PATCHED);
        return;
    }

    if( entryP->decode.memIndirect )
    {
        // DEBREAK: "jmp i 1" in bank 0 returns from a sequence-break handler
        // through the PC the hardware saved at location 1, so its target is
        // whatever the break interrupted; the assembled word there says
        // nothing about it.
        if( (entryP->bank == 0) && (entryP->decode.address == OPTDEBREAK_ADDR) )
        {
            addSinkEdge(tableP, blockP, kind, OPTSK_DEBREAK);
            return;
        }

        if( !viaJumpEdges(tableP, blockP, kind, entryP) )
        {
            addSinkEdge(tableP, blockP, kind, OPTSK_INDIRECT);
        }

        return;
    }

    addTargetEdge(tableP, blockP, kind, entryP->bank, entryP->decode.address);

    // A dac or dio that stores this word whole sends it wherever the stored
    // word says once the store has run.  Where a store does not say, the sink
    // stays beside the edges the others give, reason patched: the target is
    // written at run time, as it is by a dap.
    if( entryP->flags & OPTF_WRITTEN )
    {
        StoredTargets stored;
        int every;
        int i;

        every = storedTargets(tableP, entryP, &stored);

        for( i = 0; i < stored.count; ++i )
        {
            addTargetEdge(tableP, blockP, kind, entryP->bank, stored.addrs[i]);
            markRecovered(blockP->succTailP, OPTRV_STORED);
        }

        if( !every )
        {
            addSinkEdge(tableP, blockP, kind, OPTSK_PATCHED);
        }
    }
}

// Make the successor edges of one block, from its last word.  A block not
// ending on a terminator falls through, to the sink if nothing is there.
static void
buildBlockEdges(OptTableP tableP, OptBlockP blockP)
{
OptWordP lastP;
int next;
int skipTo;
int addr;

    lastP = blockP->lastP;
    addr = lastP->decode.address;

    // The PDP-1's address is twelve bits, so running off the top of a bank
    // wraps to the bottom of the same bank rather than entering the next one.
    next = ((blockP->endAddr + 1) & ADDRMASK);
    skipTo = ((blockP->endAddr + 2) & ADDRMASK);

    switch( blockP->endKind )
    {
    case OPTBE_SKIP:
        addTargetEdge(tableP, blockP, OPTFK_FALL, blockP->bank, next);
        addTargetEdge(tableP, blockP, OPTFK_SKIP, blockP->bank, skipTo);

        if( lastP->decode.group == OPTG_IOT )
        {
            markRecovered(blockP->succTailP, OPTRV_IOT);
        }

        break;

    case OPTBE_JUMP:
        addTransferEdges(tableP, blockP, OPTFK_JUMP, lastP);
        break;

    case OPTBE_CALL:
        if( lastP->decode.opcode == 016 )
        {
            if( lastP->flags & OPTF_PATCHED )
            {
                addSinkEdge(tableP, blockP, OPTFK_CALL, OPTSK_PATCHED);
            }
            else if( lastP->decode.indirect )
            {
                // jda deposits AC in the word it names and enters the word
                // after it.  jda never indirects: bit 5 is part of its
                // instruction code, which is why it is tested here and not
                // through addTransferEdges().
                addTargetEdge(tableP, blockP, OPTFK_CALL, lastP->bank, ((addr + 1) & ADDRMASK));
            }
            else
            {
                // cal is jda 0100: it ignores its address field entirely and
                // enters 0101 of its own bank.
                addTargetEdge(tableP, blockP, OPTFK_CALL, lastP->bank, CAL_JUMP_ADDR);
            }
        }
        else
        {
            addTransferEdges(tableP, blockP, OPTFK_CALL, lastP);
        }

        // The return edge goes where optreturn.c proved the callee returns.
        // When it proved nothing, the edge to the next word is kept and marked
        // assumed: dropping it would make the caller's tail look unreached and
        // its callees uncalled, the unsound direction for T10.
        addTargetEdge(tableP, blockP, OPTFK_RETURN, blockP->bank, lastP->returnAddr);

        if( lastP->returnCase == OPTRC_UNKNOWN )
        {
            blockP->succTailP->assumed = 1;
        }

        break;

    case OPTBE_HALT:
        // Pressing Continue on the console resumes at the next word, so the
        // word after a halt is reachable and is not dead code.
        addTargetEdge(tableP, blockP, OPTFK_RESUME, blockP->bank, next);
        break;

    case OPTBE_BOUNDARY:
    case OPTBE_NOTCODE:
    case OPTBE_GAP:
    default:
        addTargetEdge(tableP, blockP, OPTFK_FALL, blockP->bank, next);
        break;
    }
}

// Mark the entry words and their blocks.  Each is the first word of its
// block, since markBlockStarts() marked every entry cause.
static void
markEntries(OptTableP tableP)
{
int i;
OptWordP entryP;
OptEntryCause cause;

    for( i = 0; i < tableP->count; ++i )
    {
        entryP = tableP->entriesPP[i];

        if( !entryP->blockP )
        {
            continue;
        }

        if( (cause = entryCause(tableP, entryP)) == OPTEN_COUNT )
        {
            continue;
        }

        entryP->flags |= OPTF_ENTRY;
        ++tableP->entryCount;
        ++tableP->entryCounts[cause];
        entryP->blockP->isEntry = 1;
    }
}

// Walk reachability from the entry blocks over every non-sink edge.  Each
// block is pushed at most once, so the stack needs one slot per block.
static void
walkReachability(OptTableP tableP)
{
OptBlockP *stackPP;
OptBlockP blockP;
OptFlowEdgeP edgeP;
int top;

    if( !tableP->blockCount )
    {
        return;
    }

    if( !(stackPP = (OptBlockP *)calloc(tableP->blockCount, sizeof(OptBlockP))) )
    {
        fprintf(stderr, "am1: out of memory walking the optimizer control-flow graph\n");
        exit(1);
    }

    top = 0;

    for( blockP = tableP->blocksP; blockP; blockP = blockP->nextP )
    {
        if( blockP->isEntry )
        {
            blockP->reached = 1;
            stackPP[top++] = blockP;
        }
    }

    while( top > 0 )
    {
        blockP = stackPP[--top];

        for( edgeP = blockP->succP; edgeP; edgeP = edgeP->nextP )
        {
            // A sink edge means the analysis lost the thread, not that
            // control arrived anywhere, so it spreads no reachability.
            if( !edgeP->toP || edgeP->toP->reached )
            {
                continue;
            }

            edgeP->toP->reached = 1;
            stackPP[top++] = edgeP->toP;
        }
    }

    free(stackPP);

    for( blockP = tableP->blocksP; blockP; blockP = blockP->nextP )
    {
        if( blockP->reached )
        {
            ++tableP->reachedBlocks;
        }
        else
        {
            ++tableP->unreachedBlocks;
        }
    }
}

// Walk again from the blocks a jmp or a jsp names, over the blocks the first
// walk did not reach.
//
// The first walk recovers from a sink only by accident: a patched jmp, an
// unresolved indirect and a sequence-break return all spread nothing.  A
// program written with jsp-patched returns and indirect dispatch takes no
// addresses, gets one or two entries, and the walk reports almost all of it
// unreached.
//
// A block some instruction jumps to is code somebody meant to run, so a second
// walk from those blocks says which of the unreached ones are live code the
// analysis lost, rather than words nothing runs.  It is DELIBERATELY NOT the
// entry set: OPTF_ENTRY means "enterable from outside the pattern" to P1
// (optrules.c) and to S4's placement (optspeed.c), and seeding those blocks as
// entries would withdraw rewrites.  Nothing but the report reads the result.
static void
walkResumption(OptTableP tableP)
{
OptBlockP *stackPP;
OptBlockP blockP;
OptFlowEdgeP edgeP;
int top;

    if( !tableP->blockCount )
    {
        return;
    }

    if( !(stackPP = (OptBlockP *)calloc(tableP->blockCount, sizeof(OptBlockP))) )
    {
        fprintf(stderr, "am1: out of memory walking the optimizer control-flow graph\n");
        exit(1);
    }

    top = 0;

    for( blockP = tableP->blocksP; blockP; blockP = blockP->nextP )
    {
        if( blockP->reached || !blockP->firstP || !(blockP->firstP->flags & OPTF_JUMPTARGET) )
        {
            continue;
        }

        blockP->resumeSeed = 1;
        blockP->resumed = 1;
        ++tableP->resumeSeeds;
        stackPP[top++] = blockP;
    }

    // The same rule as the first walk: a sink spreads nothing, because a sink
    // is the analysis losing the thread and not control arriving anywhere.
    while( top > 0 )
    {
        blockP = stackPP[--top];

        for( edgeP = blockP->succP; edgeP; edgeP = edgeP->nextP )
        {
            if( !edgeP->toP || edgeP->toP->reached || edgeP->toP->resumed )
            {
                continue;
            }

            edgeP->toP->resumed = 1;
            stackPP[top++] = edgeP->toP;
        }
    }

    free(stackPP);

    for( blockP = tableP->blocksP; blockP; blockP = blockP->nextP )
    {
        if( blockP->resumed )
        {
            ++tableP->resumedBlocks;
        }
    }
}

// Flag the words in unreached blocks.  An xct'd word runs in place without
// control reaching it, so it is counted as executed only by xct instead.
static void
markUnreached(OptTableP tableP)
{
int i;
OptWordP entryP;

    for( i = 0; i < tableP->count; ++i )
    {
        entryP = tableP->entriesPP[i];

        if( !entryP->blockP || entryP->blockP->reached )
        {
            continue;
        }

        if( entryP->flags & OPTF_XCTTARGET )
        {
            entryP->flags |= OPTF_XCTONLY;
            ++tableP->xctOnlyWords;
        }
        else
        {
            entryP->flags |= OPTF_UNREACHED;
            ++tableP->unreachedWords;

            // Reporting only, and laid OVER OPTF_UNREACHED rather than in
            // place of it: every rule still refuses this word on the strict
            // reach, so this releases no finding.
            if( entryP->blockP->resumed )
            {
                entryP->flags |= OPTF_RESUMED;
                ++tableP->resumedWords;
            }
        }
    }
}

// Does an unreached block hold a word that is truly unreached, not merely
// executed in place by an xct?
// Returns 1 when the block has an unreached word, 0 when it has none.
static int
blockHasUnreachedWord(OptTableP tableP, OptBlockP blockP)
{
int addr;
OptWordP entryP;

    for( addr = blockP->startAddr; addr <= blockP->endAddr; ++addr )
    {
        entryP = wordAt(tableP, blockP->bank, addr);

        if( entryP && (entryP->flags & OPTF_UNREACHED) )
        {
            return(1);
        }
    }

    return(0);
}

// Release every block and its successor edges, and clear the table's list.
// Safe to call on a table whose flow was never built.
void
freeBlockList(OptTableP tableP)
{
OptBlockP blockP;
OptBlockP nextBlockP;
OptFlowEdgeP edgeP;
OptFlowEdgeP nextEdgeP;

    for( blockP = tableP->blocksP; blockP; blockP = nextBlockP )
    {
        nextBlockP = blockP->nextP;

        for( edgeP = blockP->succP; edgeP; edgeP = nextEdgeP )
        {
            nextEdgeP = edgeP->nextP;
            free(edgeP);
        }

        free(blockP);
    }

    tableP->blocksP = NILP;
    tableP->blocksTailP = NILP;
}

// Name a control-flow edge kind.
// Returns a static string, never NILP.
const char *
optFlowKindName(OptFlowKind kind)
{
    switch( kind )
    {
    case OPTFK_FALL:
        return("fall");

    case OPTFK_SKIP:
        return("skip");

    case OPTFK_JUMP:
        return("jump");

    case OPTFK_CALL:
        return("call");

    case OPTFK_RETURN:
        return("return");

    case OPTFK_RESUME:
        return("resume");

    default:
        return("unknown");
    }
}

// Name the way a block ends.
// Returns a static string, never NILP.
const char *
optBlockEndName(OptBlockEnd end)
{
    switch( end )
    {
    case OPTBE_SKIP:
        return("skip");

    case OPTBE_JUMP:
        return("jump");

    case OPTBE_CALL:
        return("call");

    case OPTBE_HALT:
        return("halt");

    case OPTBE_BOUNDARY:
        return("boundary");

    case OPTBE_NOTCODE:
        return("notcode");

    case OPTBE_GAP:
        return("gap");

    default:
        return("unknown");
    }
}

// Name the reason an edge went to the UNKNOWN sink.
// Returns a static string, never NILP.
const char *
optSinkName(OptSink sink)
{
    switch( sink )
    {
    case OPTSK_NONE:
        return("none");

    case OPTSK_INDIRECT:
        return("indirect");

    case OPTSK_PATCHED:
        return("patched");

    case OPTSK_NOWORD:
        return("noword");

    case OPTSK_NOTCODE:
        return("notcode");

    case OPTSK_OVERLAID:
        return("overlaid");

    case OPTSK_DEBREAK:
        return("debreak");

    default:
        return("unknown");
    }
}

// Name how an edge the graph once lacked was recovered.
// Returns a static string, never NILP.
const char *
optRecoveryName(OptRecovery recovered)
{
    switch( recovered )
    {
    case OPTRV_NONE:
        return("none");

    case OPTRV_IOT:
        return("iot");

    case OPTRV_STORED:
        return("stored");

    default:
        return("unknown");
    }
}

// Most patched and indirect sinks are a routine returning: "rtn, jmp ." whose
// address field the entry's dap wrote, or "jmp i rtn".  Counting them among the
// lost threads overstates what the graph lost.  A jmp the entry stores whole
// with a dac is not one: it runs the saved address as an instruction.
// Returns 1 when the sink edge leaves through a routine's return word, 0
// otherwise.
int
optSinkIsReturn(OptTableP tableP, OptBlockP blockP, OptFlowEdgeP edgeP)
{
OptWordP lastP;
OptWordP saveP;
OptRoutineP routineP;

    lastP = blockP->lastP;

    if( edgeP->toP || (edgeP->kind != OPTFK_JUMP) || !lastP ||
        (lastP->decode.group != OPTG_MEMREF) || (lastP->decode.opcode != 060) )
    {
        return(0);
    }

    for( routineP = tableP->routinesP; routineP; routineP = routineP->nextP )
    {
        if( (routineP->returnWord == OPTRW_NONE) || (routineP->returnBank != lastP->bank) )
        {
            continue;
        }

        if( edgeP->sink == OPTSK_PATCHED )
        {
            saveP = routineP->entryBlockP->firstP;

            if( (lastP->flags & OPTF_PATCHED) && (lastP->addr == routineP->returnAddr) &&
                (routineP->returnWord == OPTRW_SAVED) && saveP &&
                (saveP->decode.group == OPTG_MEMREF) && (saveP->decode.opcode == 026) )
            {
                return(1);
            }
        }
        else if( edgeP->sink == OPTSK_INDIRECT )
        {
            if( lastP->decode.memIndirect && !(lastP->flags & OPTF_PATCHED) &&
                (lastP->decode.address == routineP->returnAddr) )
            {
                return(1);
            }
        }
    }

    return(0);
}

// Name why a word is an entry of the reachability walk.
// Returns a static string, never NILP.
const char *
optEntryCauseName(OptEntryCause cause)
{
    switch( cause )
    {
    case OPTEN_START:
        return("start");

    case OPTEN_TAKEN:
        return("taken");

    case OPTEN_EXPORT:
        return("export");

    case OPTEN_SBS:
        return("sbs");

    default:
        return("unknown");
    }
}

// Print the control-flow dump (-O=flow).  In order:
//   BBAAAA VVVVVV kind    block N    FLAGS      one per word, emission order
//   block N: BBAAAA-BBAAAA W words, P in, ends END STATE | succ: EDGE, ...
//   sbs enables: ...  /  entry set: BBAAAA ...  /  unreached: BBAAAA ...
//   the return sites, as dumpReturnSites() prints them
// A reserved word prints ------ for its value, a word outside the graph "-"
// for its block, and FLAGS is "-" when none is set.  An edge prints as
// KIND->BBAAAA (" crossbank" when it leaves the bank) or KIND->sink:REASON,
// naming only the reason because most sinks have no address to print.  An
// edge the graph once lacked adds " iot" or " stored", and a sink that is a
// routine returning adds " return".
void
optDumpFlow(FILE *fP, OptTableP tableP)
{
int i;
int f;
int any;
char idBuf[16];
OptWordP entryP;
OptBlockP blockP;
OptFlowEdgeP edgeP;

    for( i = 0; i < tableP->count; ++i )
    {
        entryP = tableP->entriesPP[i];

        fprintf(fP, "%02o%04o ", entryP->bank, entryP->addr);

        if( entryP->flags & OPTF_RESERVED )
        {
            fprintf(fP, "------ %-7s ", kindName(entryP->kind));
        }
        else
        {
            fprintf(fP, "%06o %-7s ", entryP->value, kindName(entryP->kind));
        }

        if( entryP->blockP )
        {
            snprintf(idBuf, sizeof(idBuf), "%d", entryP->blockP->id);
        }
        else
        {
            snprintf(idBuf, sizeof(idBuf), "-");
        }

        fprintf(fP, "block %-4s ", idBuf);

        any = 0;

        for( f = 0; f < (int)(sizeof(flowFlagNames) / sizeof(flowFlagNames[0])); ++f )
        {
            if( entryP->flags & flowFlagNames[f].bit )
            {
                fprintf(fP, "%s%s", (any)?" ":"", flowFlagNames[f].nameP);
                any = 1;
            }
        }

        if( !any )
        {
            fprintf(fP, "-");
        }

        fprintf(fP, "\n");
    }

    for( blockP = tableP->blocksP; blockP; blockP = blockP->nextP )
    {
        fprintf(fP, "block %d: %02o%04o-%02o%04o %d words, %d in, ends %s %s%s | succ:",
            blockP->id,
            blockP->bank, blockP->startAddr, blockP->bank, blockP->endAddr,
            blockP->wordCount, blockP->predCount,
            optBlockEndName(blockP->endKind),
            (blockP->isEntry)?"entry ":"",
            (blockP->reached)?"reached":((blockP->resumed)?"unreached-resumed":"unreached"));

        any = 0;

        for( edgeP = blockP->succP; edgeP; edgeP = edgeP->nextP )
        {
            fprintf(fP, (any)?", ":" ");

            if( edgeP->toP )
            {
                fprintf(fP, "%s->%02o%04o%s", optFlowKindName(edgeP->kind),
                    edgeP->toBank, edgeP->toAddr, (edgeP->crossbank)?" crossbank":"");

                if( edgeP->recovered != OPTRV_NONE )
                {
                    fprintf(fP, " %s", optRecoveryName(edgeP->recovered));
                }
            }
            else
            {
                fprintf(fP, "%s->sink:%s%s", optFlowKindName(edgeP->kind), optSinkName(edgeP->sink),
                    (optSinkIsReturn(tableP, blockP, edgeP))?" return":"");
            }

            any = 1;
        }

        if( !any )
        {
            fprintf(fP, " -");
        }

        fprintf(fP, "\n");
    }

    // The evidence the handler entries in the entry set rest on.
    optDumpSbsEnables(fP, tableP);

    fprintf(fP, "entry set:");
    any = 0;

    for( i = 0; i < tableP->count; ++i )
    {
        entryP = tableP->entriesPP[i];

        if( entryP->flags & OPTF_ENTRY )
        {
            fprintf(fP, " %02o%04o", entryP->bank, entryP->addr);
            any = 1;
        }
    }

    fprintf(fP, "%s\n", (any)?"":" none");

    fprintf(fP, "unreached:");
    any = 0;

    for( i = 0; i < tableP->count; ++i )
    {
        entryP = tableP->entriesPP[i];

        if( entryP->flags & OPTF_UNREACHED )
        {
            fprintf(fP, " %02o%04o", entryP->bank, entryP->addr);
            any = 1;
        }
    }

    fprintf(fP, "%s\n", (any)?"":" none");

    dumpReturnSites(fP, tableP);
}

// Print the return-site part of the flow dump: a summary line, then one line
// per call site in emission order:
//   returns: N sites, A after, B stepped, C unknown; D jda or cal
//   return BBAAAA -> BBAAAA after|stepped N|unknown REASON callee BBAAAA|-
// The addresses are the call word and its return target, in the call's bank.
// Every line begins "return" so a script can pick them out.
static void
dumpReturnSites(FILE *fP, OptTableP tableP)
{
int i;
OptWordP entryP;

    fprintf(fP, "returns: %d site%s, %d after, %d stepped, %d unknown; %d jda or cal\n",
        tableP->returnSiteCount, (tableP->returnSiteCount == 1)?"":"s",
        tableP->returnCaseCounts[OPTRC_AFTER],
        tableP->returnCaseCounts[OPTRC_STEPPED],
        tableP->returnCaseCounts[OPTRC_UNKNOWN],
        tableP->returnJdaSites);

    for( i = 0; i < tableP->count; ++i )
    {
        entryP = tableP->entriesPP[i];

        if( !entryP->isCallSite )
        {
            continue;
        }

        fprintf(fP, "return %02o%04o -> %02o%04o %s",
            entryP->bank, entryP->addr,
            entryP->bank, entryP->returnAddr,
            optReturnCaseName(entryP->returnCase));

        if( entryP->returnCase == OPTRC_STEPPED )
        {
            fprintf(fP, " %d", entryP->returnSteps);
        }
        else if( entryP->returnCase == OPTRC_UNKNOWN )
        {
            fprintf(fP, " %s", optReturnReasonName(entryP->returnReason));
        }

        if( entryP->calleeAddr >= 0 )
        {
            fprintf(fP, " callee %02o%04o\n", entryP->calleeBank, entryP->calleeAddr);
        }
        else
        {
            fprintf(fP, " callee -\n");
        }
    }
}

// Write one bank's control-flow section of the report: what the graph holds
// there, the edges it made, and what the reachability walk found.  Silent
// about a bank with no code words beyond saying so, since every figure would
// be zero.
void
writeFlowReport(FILE *fP, OptTableP tableP, int bank)
{
int i;
int blocks;
int words;
int largest;
int entries;
int reached;
int unreachedBlocks;
int unreachedWords;
int resumedBlocks;
int resumedWords;
int xctOnly;
int any;
int kindCounts[OPTFK_COUNT];
int sinkCounts[OPTSK_COUNT];
int returnSinks[OPTSK_COUNT];
int recoveredCounts[OPTRV_COUNT];
int entryCounts[OPTEN_COUNT];
OptBlockP blockP;
OptFlowEdgeP edgeP;
OptWordP entryP;
OptEntryCause cause;

    blocks = 0;
    words = 0;
    largest = 0;
    entries = 0;
    reached = 0;
    unreachedBlocks = 0;
    unreachedWords = 0;
    resumedBlocks = 0;
    resumedWords = 0;
    xctOnly = 0;

    for( i = 0; i < OPTFK_COUNT; ++i )
    {
        kindCounts[i] = 0;
    }

    for( i = 0; i < OPTSK_COUNT; ++i )
    {
        sinkCounts[i] = 0;
        returnSinks[i] = 0;
    }

    for( i = 0; i < OPTRV_COUNT; ++i )
    {
        recoveredCounts[i] = 0;
    }

    for( i = 0; i < OPTEN_COUNT; ++i )
    {
        entryCounts[i] = 0;
    }

    for( blockP = tableP->blocksP; blockP; blockP = blockP->nextP )
    {
        if( blockP->bank != bank )
        {
            continue;
        }

        ++blocks;
        words += blockP->wordCount;

        if( blockP->wordCount > largest )
        {
            largest = blockP->wordCount;
        }

        if( blockP->reached )
        {
            ++reached;
        }
        else
        {
            ++unreachedBlocks;

            if( blockP->resumed )
            {
                ++resumedBlocks;
            }
        }

        for( edgeP = blockP->succP; edgeP; edgeP = edgeP->nextP )
        {
            ++kindCounts[edgeP->kind];
            ++recoveredCounts[edgeP->recovered];

            if( !edgeP->toP )
            {
                ++sinkCounts[edgeP->sink];

                if( optSinkIsReturn(tableP, blockP, edgeP) )
                {
                    ++returnSinks[edgeP->sink];
                }
            }
        }
    }

    for( i = 0; i < tableP->count; ++i )
    {
        entryP = tableP->entriesPP[i];

        if( entryP->bank != bank )
        {
            continue;
        }

        if( entryP->flags & OPTF_ENTRY )
        {
            ++entries;

            if( (cause = entryCause(tableP, entryP)) != OPTEN_COUNT )
            {
                ++entryCounts[cause];
            }
        }

        if( entryP->flags & OPTF_UNREACHED )
        {
            ++unreachedWords;

            if( entryP->flags & OPTF_RESUMED )
            {
                ++resumedWords;
            }
        }

        if( entryP->flags & OPTF_XCTONLY )
        {
            ++xctOnly;
        }
    }

    if( !blocks )
    {
        fprintf(fP, "    flow: no code words, no blocks\n");
        return;
    }

    fprintf(fP, "    flow: %d block%s, %d word%s, largest %d; edges:",
        blocks, (blocks == 1)?"":"s", words, (words == 1)?"":"s", largest);

    for( i = 0; i < OPTFK_COUNT; ++i )
    {
        if( kindCounts[i] )
        {
            fprintf(fP, " %s %d", optFlowKindName((OptFlowKind)i), kindCounts[i]);
        }
    }

    any = 0;

    for( i = (OPTRV_NONE + 1); i < OPTRV_COUNT; ++i )
    {
        if( recoveredCounts[i] )
        {
            fprintf(fP, "%s %s %d", (any)?"":"; recovered:",
                optRecoveryName((OptRecovery)i), recoveredCounts[i]);
            any = 1;
        }
    }

    any = 0;

    for( i = 0; i < OPTSK_COUNT; ++i )
    {
        if( sinkCounts[i] )
        {
            fprintf(fP, "%s %s %d", (any)?"":"; to the unknown sink:",
                optSinkName((OptSink)i), sinkCounts[i]);
            any = 1;
        }
    }

    // Of those, the ones a routine returns through: every call site keeps its
    // own edge back, so these lose nothing and the rest are the lost threads.
    any = 0;

    for( i = 0; i < OPTSK_COUNT; ++i )
    {
        if( returnSinks[i] )
        {
            fprintf(fP, "%s %s %d", (any)?"":", of which routine returns:",
                optSinkName((OptSink)i), returnSinks[i]);
            any = 1;
        }
    }

    fprintf(fP, "\n    reachability: %d entries", entries);

    for( i = 0; i < OPTEN_COUNT; ++i )
    {
        if( entryCounts[i] )
        {
            fprintf(fP, " %s %d", optEntryCauseName((OptEntryCause)i), entryCounts[i]);
        }
    }

    fprintf(fP, "; %d blocks reached, %d unreached holding %d word%s",
        reached, unreachedBlocks, unreachedWords, (unreachedWords == 1)?"":"s");

    // Of the unreached, the ones some jmp or jsp names: live code the walk
    // lost rather than words nothing runs.  Every rule still treats them as
    // unreached, so this line changes what a reader believes and nothing else.
    if( resumedBlocks )
    {
        fprintf(fP, ", of which %d block%s holding %d word%s named by a jmp or jsp",
            resumedBlocks, (resumedBlocks == 1)?"":"s", resumedWords,
            (resumedWords == 1)?"":"s");
    }

    if( xctOnly )
    {
        fprintf(fP, ", %d word%s executed only by xct", xctOnly, (xctOnly == 1)?"":"s");
    }

    fprintf(fP, "\n");
}

// Write the apparently-unreached section at the end of the report, one line
// per unreached block, with the caveat that makes the list readable: it is a
// hint and there are several ordinary reasons a live routine lands in it.
// Blocks whose every word is executed by an xct are not listed; they are
// executed, and calling them unreached code would simply be wrong.
void
writeUnreachedList(FILE *fP, OptTableP tableP)
{
int blocks;
OptBlockP blockP;

    blocks = 0;

    for( blockP = tableP->blocksP; blockP; blockP = blockP->nextP )
    {
        if( !blockP->reached && blockHasUnreachedWord(tableP, blockP) )
        {
            ++blocks;
        }
    }

    if( !blocks )
    {
        return;
    }

    fprintf(fP, "\nApparently unreached code: %d block%s, %d word%s\n",
        blocks, (blocks == 1)?"":"s",
        tableP->unreachedWords, (tableP->unreachedWords == 1)?"":"s");
    fprintf(fP, "  A hint, not a proof.  A dispatch table of jmp words is read as data and\n");
    fprintf(fP, "  is never followed as a jump, so a routine only ever entered through one\n");
    fprintf(fP, "  appears here; so does anything reached only through a pointer that could\n");
    fprintf(fP, "  not be followed, or through a patched address field.  Check against the\n");
    fprintf(fP, "  source before believing any of it.\n");
    // The figures are the ones the -O=flow returns section prints; the
    // remaining caveat is the unknown case, whose return edge is only assumed.
    if( tableP->returnSiteCount )
    {
        fprintf(fP, "  Where a call returns to is asked of the callee, not assumed: of %d call\n",
            tableP->returnSiteCount);
        fprintf(fP, "  site%s, %d return to the word after the call and %d step past inline\n",
            (tableP->returnSiteCount == 1)?"":"s",
            tableP->returnCaseCounts[OPTRC_AFTER],
            tableP->returnCaseCounts[OPTRC_STEPPED]);
        fprintf(fP, "  argument words, both proved by walking every path through the callee.\n");
    }

    if( tableP->returnCaseCounts[OPTRC_UNKNOWN] )
    {
        fprintf(fP, "  The other %d could not be read -- a callee reached only through a pointer\n",
            tableP->returnCaseCounts[OPTRC_UNKNOWN]);
        fprintf(fP, "  nothing followed, a return word written by something this analysis cannot\n");
        fprintf(fP, "  follow, or paths that disagree -- and each of those keeps the assumed edge\n");
        fprintf(fP, "  to the word after the call.  A caller of one of them that really does pass\n");
        fprintf(fP, "  an inline argument therefore still loses its return, and everything below\n");
        fprintf(fP, "  the call still appears here; \"-O=flow\" names the sites and the reason for\n");
        fprintf(fP, "  each.\n");
    }

    fprintf(fP, "  One caveat the other way: an xct is not treated as a terminator, so a word\n");
    fprintf(fP, "  after an xct of a jump is called reached when it may never run.\n");

    // A block some jmp or jsp names is code somebody meant to run, whether or
    // not the walk could get to the naming word.  Marked on the rows below.
    if( tableP->resumedBlocks )
    {
        fprintf(fP, "  Marked \"jmp/jsp target\" below: %d block%s holding %d word%s.  Some\n",
            tableP->resumedBlocks, (tableP->resumedBlocks == 1)?"":"s",
            tableP->resumedWords, (tableP->resumedWords == 1)?"":"s");
        fprintf(fP, "  instruction names each of those blocks, or names a block that runs into\n");
        fprintf(fP, "  one, so they are live code this walk lost rather than words nothing runs.\n");
        fprintf(fP, "  Every rule and heuristic still treats them as unreached, which refuses\n");
        fprintf(fP, "  findings there.\n");
    }

    for( blockP = tableP->blocksP; blockP; blockP = blockP->nextP )
    {
        if( blockP->reached || !blockHasUnreachedWord(tableP, blockP) )
        {
            continue;
        }

        fprintf(fP, "    bank %2d %04o-%04o, %d word%s, line %d, first label %s%s\n",
            blockP->bank, blockP->startAddr, blockP->endAddr,
            blockP->wordCount, (blockP->wordCount == 1)?"":"s",
            blockP->firstP->lineNo, firstLabelName(blockP->firstP),
            (blockP->resumed)?"  jmp/jsp target":"");
    }
}
