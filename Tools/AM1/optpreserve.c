/*
 * The am1 optimizer's register summaries, for the carry measurement (task
 * A52, -O=carry): for every routine, whether it returns IO as it found it,
 * whether a jda routine returns the caller's AC, whether it reads IO's entry
 * value, and which memory words it and its callees write and read.  And the
 * call step: what the caller knows after a call returns.  A measurement only:
 * nothing here makes a finding or changes a word, and no level reads it.
 *
 * A summary is proved by running the value pass over the routine's body
 * (optCarryBody() in optvalue.c) with IO, and for a jda routine the word jda
 * deposits AC in, holding a symbol at the entry, and asking every return word
 * whether the symbol is still there.  jsp puts the return point in AC before
 * the routine runs, so the caller's AC can only survive a jda or cal call.
 * Routines are summarized callees first: by call depth, then by strongly
 * connected component, whose members are iterated together for what they
 * read and write.  A register not proved gets one reason, the first of
 * clobbers, iot, calls, jumps, cycle, noreturn, open and handler that
 * applies; the first three are told apart by running the body again with in-out
 * transfers, calls or both taken as harmless.
 *
 * Word sets are flags per bank, made only for a bank something touches.
 * Allocation failure is fatal, as elsewhere in am1.
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

// The memory reference operation codes used here, as optdecode.c numbers them.
#define PROP_JMP        0060
#define PROP_JDA        0016    // jda with bit 5 set, cal without

// cal deposits AC in 0100 whatever its address field says.
#define PRO_CALSAVE     0100

// The runs made over each body, by what they take as harmless.
#define PRUN_PROOF      0
#define PRUN_OPTIMISTIC (OPTCM_IOTFREE | OPTCM_CALLSOPT)
#define PRUN_IOTTEST    (OPTCM_CALLSOPT)
#define PRUN_CALLTEST   (OPTCM_IOTFREE | OPTCM_SCCOPT)
#define PRUN_FREE       (OPTCM_IOTFREE | OPTCM_FREESUMS)
#define PRUN_FREECALL   (OPTCM_IOTFREE | OPTCM_FREESUMS | OPTCM_SCCOPT)

static const char *whyNames[OPTPR_COUNT] =
{
    "yes", "jsp", "clobbers", "iot", "calls", "jumps", "cycle", "noreturn", "open", "handler", "unexplained"
};

static const char *setWhyNames[OPTPW_COUNT] =
{
    "-", "indirect", "xct", "iot", "spare", "calls"
};

// Allocate zeroed memory; out of memory is fatal.
// Returns the memory, never NILP.
static void *
pAlloc(size_t count, size_t size)
{
void *memP;

    if( !(memP = calloc((count)?count:1, size)) )
    {
        fprintf(stderr, "am1: out of memory summarizing routines\n");
        exit(1);
    }

    return(memP);
}

// The name of a reason.
// Returns a static string.
const char *
optPreserveWhyName(OptPreserveWhy why)
{
    return( ((why >= 0) && (why < OPTPR_COUNT))?whyNames[why]:"?" );
}

// The name of why a set is any word.
// Returns a static string.
const char *
optWordSetWhyName(OptWordSetWhy why)
{
    return( ((why >= 0) && (why < OPTPW_COUNT))?setWhyNames[why]:"?" );
}

// Word sets.

// Add one word to a set.
static void
setAdd(OptWordSet *setP, int bank, int addr)
{
    if( (bank < 0) || (bank > MAXBANK) || (addr < 0) || (addr >= BANKSIZE) )
    {
        return;
    }

    if( !setP->bankP[bank] )
    {
        setP->bankP[bank] = (unsigned char *)pAlloc(BANKSIZE, 1);
    }

    if( !setP->bankP[bank][addr] )
    {
        setP->bankP[bank][addr] = 1;
        ++setP->count;
    }
}

// Make a set any word, keeping the first reason.
static void
setAny(OptWordSet *setP, OptWordSetWhy why)
{
    if( setP->any == OPTPW_NONE )
    {
        setP->any = why;
    }
}

// Add every word of one set to another, and its "any".
static void
setUnion(OptWordSet *intoP, const OptWordSet *fromP)
{
int bank;
int addr;

    if( fromP->any != OPTPW_NONE )
    {
        setAny(intoP, fromP->any);
    }

    for( bank = 0; bank <= MAXBANK; ++bank )
    {
        if( !fromP->bankP[bank] )
        {
            continue;
        }

        for( addr = 0; addr < BANKSIZE; ++addr )
        {
            if( fromP->bankP[bank][addr] )
            {
                setAdd(intoP, bank, addr);
            }
        }
    }
}

// Empty a set and release its banks.
static void
setFree(OptWordSet *setP)
{
int bank;

    for( bank = 0; bank <= MAXBANK; ++bank )
    {
        free(setP->bankP[bank]);
        setP->bankP[bank] = NILP;
    }

    setP->any = OPTPW_NONE;
    setP->count = 0;
}

// Is a word in a set?
// Returns 1 when it is or the set is any word, 0 otherwise.
int
optWordSetHas(const OptWordSet *setP, int bank, int addr)
{
    if( setP->any != OPTPW_NONE )
    {
        return(1);
    }

    if( (bank < 0) || (bank > MAXBANK) || (addr < 0) || (addr >= BANKSIZE) || !setP->bankP[bank] )
    {
        return(0);
    }

    return( setP->bankP[bank][addr] );
}

// The lookups.

// Index the blocks by id and the routines by their entry block.
void
optCarryMapsBuild(OptTableP tableP, OptCarryMapsP mapsP)
{
OptBlockP blockP;
OptRoutineP routineP;

    mapsP->blockOfIdPP = (OptBlockP *)pAlloc((size_t)(tableP->blockCount + 1), sizeof(OptBlockP));
    mapsP->routineOfEntryPP = (OptRoutineP *)pAlloc((size_t)(tableP->blockCount + 1), sizeof(OptRoutineP));

    for( blockP = tableP->blocksP; blockP; blockP = blockP->nextP )
    {
        if( (blockP->id > 0) && (blockP->id <= tableP->blockCount) )
        {
            mapsP->blockOfIdPP[blockP->id] = blockP;
        }
    }

    for( routineP = tableP->routinesP; routineP; routineP = routineP->nextP )
    {
        if( routineP->entryBlockP )
        {
            mapsP->routineOfEntryPP[routineP->entryBlockP->id] = routineP;
        }
    }
}

// Release the lookups.
void
optCarryMapsFree(OptCarryMapsP mapsP)
{
    free(mapsP->blockOfIdPP);
    free(mapsP->routineOfEntryPP);
    mapsP->blockOfIdPP = NILP;
    mapsP->routineOfEntryPP = NILP;
}

// Is a word one of a routine's return words?  The test optroutine.c's
// isReturnWord() makes: "jmp i rtn" naming the return word, or the patched
// "rtn, jmp ." at the return word's address, tested by address.
// Returns 1 when it is, 0 otherwise.
int
optPreserveIsReturn(OptRoutineP routineP, OptWordP wordP)
{
    if( !wordP || !inGraph(wordP) || (routineP->returnAddr < 0) )
    {
        return(0);
    }

    if( (wordP->bank == routineP->returnBank) && (wordP->addr == routineP->returnAddr) )
    {
        return( (wordP->decode.group == OPTG_MEMREF) && (wordP->decode.opcode == PROP_JMP) );
    }

    if( (wordP->decode.group != OPTG_MEMREF) || (wordP->decode.opcode != PROP_JMP) || !wordP->decode.memIndirect )
    {
        return(0);
    }

    if( wordP->flags & OPTF_PATCHED )
    {
        return(0);
    }

    return( (wordP->bank == routineP->returnBank) && (wordP->decode.address == routineP->returnAddr) );
}

// What a routine's own words write and read, before its callees are added.
// A write or read through an unfollowed pointer, an xct, a spare code and an
// unknown in-out transfer make the set any word; the direct set, leg 3's,
// takes only the words named.  The free sets are the plain ones but for
// in-out transfers.
static void
ownSets(OptTableP tableP, OptCarryMapsP mapsP, OptPreserveP sumP)
{
OptRoutineP routineP;
OptBlockP blockP;
OptWordP wordP;
OptEdgeP edgeP;
OptFlowEdgeP flowP;
int i;
int addr;
int kills;

    routineP = sumP->routineP;

    for( i = 0; i < routineP->blockCount; ++i )
    {
        if( !(blockP = mapsP->blockOfIdPP[routineP->bodyIdsP[i]]) )
        {
            continue;
        }

        for( flowP = blockP->succP; flowP; flowP = flowP->nextP )
        {
            if( (flowP->kind == OPTFK_CALL) && !flowP->toP )
            {
                setAny(&sumP->writes, OPTPW_CALLS);
                setAny(&sumP->reads, OPTPW_CALLS);
                setAny(&sumP->freeWrites, OPTPW_CALLS);
                setAny(&sumP->freeReads, OPTPW_CALLS);
            }
        }

        for( addr = blockP->startAddr; addr <= blockP->endAddr; ++addr )
        {
            if( !(wordP = wordAt(tableP, blockP->bank, addr)) || (wordP->blockP != blockP) )
            {
                continue;
            }

            kills = optValueKills(wordP);

            if( kills == OPTPW_IOT )
            {
                setAny(&sumP->writes, OPTPW_IOT);
                setAny(&sumP->reads, OPTPW_IOT);
            }
            else if( kills != OPTPW_NONE )
            {
                setAny(&sumP->writes, (OptWordSetWhy)kills);
                setAny(&sumP->reads, (OptWordSetWhy)kills);
                setAny(&sumP->freeWrites, (OptWordSetWhy)kills);
                setAny(&sumP->freeReads, (OptWordSetWhy)kills);
            }

            for( edgeP = wordP->outP; edgeP; edgeP = edgeP->nextOutP )
            {
                if( edgeP->flags & OPTEF_PLACEHOLDER )
                {
                    continue;
                }

                if( edgeP->role == OPTR_WRITE )
                {
                    if( edgeP->flags & OPTEF_INDIRECT )
                    {
                        // The pointer word is read; the write lands on the
                        // followed edge, or somewhere unknown.
                        setAdd(&sumP->reads, edgeP->toBank, edgeP->toAddr);
                        setAdd(&sumP->freeReads, edgeP->toBank, edgeP->toAddr);

                        if( edgeP->flags & OPTEF_UNKNOWN )
                        {
                            setAny(&sumP->writes, OPTPW_INDIRECT);
                            setAny(&sumP->freeWrites, OPTPW_INDIRECT);
                        }

                        continue;
                    }

                    setAdd(&sumP->writes, edgeP->toBank, edgeP->toAddr);
                    setAdd(&sumP->freeWrites, edgeP->toBank, edgeP->toAddr);
                    setAdd(&sumP->direct, edgeP->toBank, edgeP->toAddr);
                }
                else if( edgeP->role == OPTR_READ )
                {
                    setAdd(&sumP->reads, edgeP->toBank, edgeP->toAddr);
                    setAdd(&sumP->freeReads, edgeP->toBank, edgeP->toAddr);

                    if( (edgeP->flags & OPTEF_INDIRECT) && (edgeP->flags & OPTEF_UNKNOWN) )
                    {
                        setAny(&sumP->reads, OPTPW_INDIRECT);
                        setAny(&sumP->freeReads, OPTPW_INDIRECT);
                    }
                }
            }
        }
    }
}

// What a component's iteration watches: every set only grows and every read
// of IO only turns on, so a sum of their sizes changes exactly when one does.
// Returns the fingerprint.
static long
fingerprint(const OptPreserve *sumP)
{
    return( ((long)sumP->writes.count * 7919L) + ((long)sumP->reads.count * 104729L)
            + ((long)sumP->freeWrites.count * 13L) + ((long)sumP->freeReads.count * 31L)
            + ((long)sumP->direct.count * 3L)
            + ((long)sumP->writes.any * 1000003L) + ((long)sumP->reads.any * 2000003L)
            + ((long)sumP->freeWrites.any * 3000017L) + ((long)sumP->freeReads.any * 4000037L)
            + ((long)sumP->ioRead * 5000011L) + ((long)sumP->freeIoRead * 6000101L) );
}

// One routine's sets, from its own and its callees' as they now stand.
static void
gatherSets(OptTableP tableP, OptCarryMapsP mapsP, OptPreserveP sumsP, OptPreserveP sumP)
{
OptCallArcP arcP;
OptPreserveP calleeP;

    setFree(&sumP->writes);
    setFree(&sumP->reads);
    setFree(&sumP->direct);
    setFree(&sumP->freeWrites);
    setFree(&sumP->freeReads);
    ownSets(tableP, mapsP, sumP);

    for( arcP = sumP->routineP->callsP; arcP; arcP = arcP->nextP )
    {
        calleeP = &sumsP[arcP->toP->id];

        if( !calleeP->done )
        {
            // Not made yet and not in this component: the order is wrong
            // somewhere, so the callee might do anything.
            setAny(&sumP->writes, OPTPW_CALLS);
            setAny(&sumP->reads, OPTPW_CALLS);
            setAny(&sumP->freeWrites, OPTPW_CALLS);
            setAny(&sumP->freeReads, OPTPW_CALLS);
            continue;
        }

        setUnion(&sumP->writes, &calleeP->writes);
        setUnion(&sumP->reads, &calleeP->reads);
        setUnion(&sumP->direct, &calleeP->direct);
        setUnion(&sumP->freeWrites, &calleeP->freeWrites);
        setUnion(&sumP->freeReads, &calleeP->freeReads);
    }

    // The direct set is never any: leg 3 counts only what is named.
    sumP->direct.any = OPTPW_NONE;
}

// The reason a register is not preserved, the first that applies, or
// OPTPR_YES.  proof, optimistic, iotTest and callTest say whether each run
// kept the register at every return; iotTest < 0 skips the iot reason.
// Returns the reason.
static OptPreserveWhy
reasonFor(OptRoutineP routineP, int returns, int proof, int optimistic, int iotTest, int callTest)
{
unsigned int flags;

    flags = routineP->flags;

    if( proof && returns && !(flags & (OPTRT_JUMPSUNKNOWN | OPTRT_ONCYCLE | OPTRT_NORETURN | OPTRT_FALLIN
                                       | OPTRT_OPENBODY | OPTRT_SHARED | OPTRT_SPANHOLE | OPTRT_SBSPOOL)) )
    {
        return(OPTPR_YES);
    }

    if( returns && !optimistic )
    {
        return(OPTPR_CLOBBERS);
    }

    if( returns && (iotTest == 0) )
    {
        return(OPTPR_IOT);
    }

    if( returns && !callTest )
    {
        return(OPTPR_CALLS);
    }

    if( flags & OPTRT_JUMPSUNKNOWN )
    {
        return(OPTPR_JUMPS);
    }

    if( flags & OPTRT_ONCYCLE )
    {
        return(OPTPR_CYCLE);
    }

    if( (flags & OPTRT_NORETURN) || !returns )
    {
        return(OPTPR_NORETURN);
    }

    if( flags & (OPTRT_FALLIN | OPTRT_OPENBODY | OPTRT_SHARED | OPTRT_SPANHOLE) )
    {
        return(OPTPR_OPEN);
    }

    if( flags & OPTRT_SBSPOOL )
    {
        return(OPTPR_HANDLER);
    }

    return(OPTPR_UNEXPLAINED);
}

// Prove one routine's registers: the plain runs and the free ones, then a
// reason for each register not proved.
static void
proveRegisters(OptTableP tableP, OptCarryMapsP mapsP, OptPreserveP sumsP, OptPreserveP sumP)
{
OptRoutineP routineP;
OptBodyRun proof;
OptBodyRun optimistic;
OptBodyRun iotTest;
OptBodyRun callTest;
OptBodyRun free;
OptBodyRun freeCall;
int jda;

    routineP = sumP->routineP;
    optCarryBody(tableP, mapsP, sumsP, routineP, PRUN_PROOF, &proof);
    optCarryBody(tableP, mapsP, sumsP, routineP, PRUN_OPTIMISTIC, &optimistic);
    optCarryBody(tableP, mapsP, sumsP, routineP, PRUN_IOTTEST, &iotTest);
    optCarryBody(tableP, mapsP, sumsP, routineP, PRUN_CALLTEST, &callTest);
    optCarryBody(tableP, mapsP, sumsP, routineP, PRUN_FREE, &free);
    optCarryBody(tableP, mapsP, sumsP, routineP, PRUN_FREECALL, &freeCall);

    sumP->io = reasonFor(routineP, proof.returns, proof.ioKept, optimistic.ioKept, iotTest.ioKept, callTest.ioKept);
    sumP->freeIo = reasonFor(routineP, free.returns, free.ioKept, optimistic.ioKept, -1, freeCall.ioKept);

    // jsp, or a caller that uses it, overwrites AC at the call.
    jda = (routineP->form == OPTRF_JDA);

    if( !jda )
    {
        sumP->ac = OPTPR_JSP;
        sumP->freeAc = OPTPR_JSP;
    }
    else
    {
        sumP->ac = reasonFor(routineP, proof.returns, proof.acKept, optimistic.acKept, iotTest.acKept, callTest.acKept);
        sumP->freeAc = reasonFor(routineP, free.returns, free.acKept, optimistic.acKept, -1, freeCall.acKept);
    }
}

// The order routines are summarized in: callees first, which is ascending call
// depth, a component's members together.
// Returns negative, zero or positive for qsort().
static int
compareOrder(const void *aP, const void *bP)
{
const OptRoutine *aR;
const OptRoutine *bR;

    aR = *(const OptRoutine * const *)aP;
    bR = *(const OptRoutine * const *)bP;

    if( aR->callDepth != bR->callDepth )
    {
        return( aR->callDepth - bR->callDepth );
    }

    if( aR->scc != bR->scc )
    {
        return( aR->scc - bR->scc );
    }

    return( aR->id - bR->id );
}

// Make every routine's summary.
// Returns routineCount + 1 summaries by routine id, index 0 unused.
OptPreserveP
optPreserveBuild(OptTableP tableP, OptCarryMapsP mapsP)
{
OptPreserveP sumsP;
OptPreserveP sumP;
OptRoutineP routineP;
OptRoutineP *orderPP;
OptBodyRun run;
int count;
int i;
int first;
int last;
int changed;
int rounds;
long before;

    sumsP = (OptPreserveP)pAlloc((size_t)(tableP->routineCount + 1), sizeof(OptPreserve));
    orderPP = (OptRoutineP *)pAlloc((size_t)(tableP->routineCount + 1), sizeof(OptRoutineP));
    count = 0;

    for( routineP = tableP->routinesP; routineP; routineP = routineP->nextP )
    {
        sumsP[routineP->id].routineP = routineP;
        sumsP[routineP->id].io = OPTPR_UNEXPLAINED;
        sumsP[routineP->id].ac = OPTPR_UNEXPLAINED;
        sumsP[routineP->id].freeIo = OPTPR_UNEXPLAINED;
        sumsP[routineP->id].freeAc = OPTPR_UNEXPLAINED;
        orderPP[count++] = routineP;
    }

    qsort(orderPP, (size_t)count, sizeof(OptRoutineP), compareOrder);

    for( first = 0; first < count; first = last )
    {
        // One component: the routines with its depth and scc.  A routine with
        // no scc (0) is a component of its own.
        for( last = first + 1; last < count; ++last )
        {
            if( !orderPP[first]->scc || (orderPP[last]->scc != orderPP[first]->scc)
                || (orderPP[last]->callDepth != orderPP[first]->callDepth) )
            {
                break;
            }
        }

        // Its members are made at once, knowing nothing they read or write,
        // and iterated until what they read, write and read of IO is stable:
        // each round can only add.
        for( i = first; i < last; ++i )
        {
            sumsP[orderPP[i]->id].done = 1;
        }

        rounds = 0;

        do
        {
            changed = 0;

            for( i = first; i < last; ++i )
            {
                sumP = &sumsP[orderPP[i]->id];
                before = fingerprint(sumP);
                gatherSets(tableP, mapsP, sumsP, sumP);
                optCarryBody(tableP, mapsP, sumsP, sumP->routineP, PRUN_PROOF, &run);
                sumP->ioRead = (sumP->ioRead || run.ioRead);
                optCarryBody(tableP, mapsP, sumsP, sumP->routineP, PRUN_FREE, &run);
                sumP->freeIoRead = (sumP->freeIoRead || run.ioRead);

                if( fingerprint(sumP) != before )
                {
                    changed = 1;
                }
            }

            // A component of one that does not call itself is done in a round:
            // nothing it reads changed under it.
            if( ((last - first) == 1) && !(orderPP[first]->flags & OPTRT_ONCYCLE) )
            {
                changed = 0;
            }
        } while( changed && (++rounds < 64) );

        for( i = first; i < last; ++i )
        {
            proveRegisters(tableP, mapsP, sumsP, &sumsP[orderPP[i]->id]);
        }
    }

    free(orderPP);
    return(sumsP);
}

// Release the summaries.
void
optPreserveFree(OptTableP tableP, OptPreserveP sumsP)
{
int i;

    if( !sumsP )
    {
        return;
    }

    for( i = 0; i <= tableP->routineCount; ++i )
    {
        setFree(&sumsP[i].writes);
        setFree(&sumsP[i].reads);
        setFree(&sumsP[i].direct);
        setFree(&sumsP[i].freeWrites);
        setFree(&sumsP[i].freeReads);
    }

    free(sumsP);
}

// A routine's name for the dump: its entry's label, or for a jda routine
// whose entry has none, the label of the word below it, where jda deposits
// AC, with "+1"; "-" when neither has one.
static void
routineName(OptTableP tableP, OptRoutineP routineP, char *bufP, int size)
{
OptWordP entryP;
OptWordP belowP;
const char *nameP;

    entryP = routineP->entryBlockP->firstP;
    nameP = firstLabelName(entryP);

    if( strcmp(nameP, "-") )
    {
        snprintf(bufP, (size_t)size, "%s", nameP);
        return;
    }

    if( (belowP = wordAt(tableP, routineP->bank, ((routineP->entryAddr - 1) & ADDRMASK)))
        && strcmp(firstLabelName(belowP), "-") )
    {
        snprintf(bufP, (size_t)size, "%s+1", firstLabelName(belowP));
        return;
    }

    snprintf(bufP, (size_t)size, "-");
}

// Spell a register's summary.
static void
spellWhy(OptPreserveWhy why, char *bufP, int size)
{
    if( (why == OPTPR_YES) || (why == OPTPR_JSP) )
    {
        snprintf(bufP, (size_t)size, "%s", optPreserveWhyName(why));
    }
    else
    {
        snprintf(bufP, (size_t)size, "no:%s", optPreserveWhyName(why));
    }
}

// Spell a word set's size.
static void
spellSet(const OptWordSet *setP, char *bufP, int size)
{
    if( setP->any != OPTPW_NONE )
    {
        snprintf(bufP, (size_t)size, "any:%s", optWordSetWhyName(setP->any));
    }
    else
    {
        snprintf(bufP, (size_t)size, "%d", setP->count);
    }
}

// Print the summaries: one line per routine, in id order, then the counts.
//
//   routine NAME io X ac Y ioread Z writes W free io X ac Y writes W | bB ADDR form F returns N direct D
//
// X and Y are yes, jsp or no:REASON; W is a count of words or any:REASON.
void
optPreserveDump(FILE *fP, OptTableP tableP, OptPreserveP sumsP)
{
OptRoutineP routineP;
OptPreserveP sumP;
char name[OPTMSG_SIZE];
char io[32];
char ac[32];
char writes[32];
char freeIo[32];
char freeAc[32];
char freeWrites[32];
int ioWhy[OPTPR_COUNT];
int acWhy[OPTPR_COUNT];
int freeIoWhy[OPTPR_COUNT];
int jdaCount;
int ioRead;
int anyWrites;
int i;

    memset(ioWhy, 0, sizeof(ioWhy));
    memset(acWhy, 0, sizeof(acWhy));
    memset(freeIoWhy, 0, sizeof(freeIoWhy));
    jdaCount = 0;
    ioRead = 0;
    anyWrites = 0;

    for( routineP = tableP->routinesP; routineP; routineP = routineP->nextP )
    {
        sumP = &sumsP[routineP->id];
        routineName(tableP, routineP, name, sizeof(name));
        spellWhy(sumP->io, io, sizeof(io));
        spellWhy(sumP->ac, ac, sizeof(ac));
        spellSet(&sumP->writes, writes, sizeof(writes));
        spellWhy(sumP->freeIo, freeIo, sizeof(freeIo));
        spellWhy(sumP->freeAc, freeAc, sizeof(freeAc));
        spellSet(&sumP->freeWrites, freeWrites, sizeof(freeWrites));
        fprintf(fP, "routine %s io %s ac %s ioread %s writes %s free io %s ac %s writes %s"
            " | b%d %04o form %s returns %d direct %d\n",
            name, io, ac, (sumP->ioRead)?"yes":"no", writes, freeIo, freeAc, freeWrites,
            routineP->bank, routineP->entryAddr, optRoutineFormName(routineP->form), routineP->returnCount,
            sumP->direct.count);

        ++ioWhy[sumP->io];
        ++freeIoWhy[sumP->freeIo];

        if( routineP->form == OPTRF_JDA )
        {
            ++jdaCount;
            ++acWhy[sumP->ac];
        }

        ioRead += sumP->ioRead;
        anyWrites += (sumP->writes.any != OPTPW_NONE);
    }

    fprintf(fP, "summary: %d routines; io", tableP->routineCount);

    for( i = 0; i < OPTPR_COUNT; ++i )
    {
        if( i != OPTPR_JSP )
        {
            fprintf(fP, " %s %d", optPreserveWhyName((OptPreserveWhy)i), ioWhy[i]);
        }
    }

    fprintf(fP, "\nsummary: %d jda routines; ac", jdaCount);

    for( i = 0; i < OPTPR_COUNT; ++i )
    {
        if( i != OPTPR_JSP )
        {
            fprintf(fP, " %s %d", optPreserveWhyName((OptPreserveWhy)i), acWhy[i]);
        }
    }

    fprintf(fP, "\nsummary: free io");

    for( i = 0; i < OPTPR_COUNT; ++i )
    {
        if( i != OPTPR_JSP )
        {
            fprintf(fP, " %s %d", optPreserveWhyName((OptPreserveWhy)i), freeIoWhy[i]);
        }
    }

    fprintf(fP, "\nsummary: ioread %d, writes any %d\n", ioRead, anyWrites);
}

// The call step.  From the caller's state just before the call word: jda and
// cal deposit AC first; then the callee keeps IO, and AC for jda and cal,
// when its summary says so (or the leg or run takes it as given), and every
// fact about a word it may write dies.  A call to the sink knows nothing.
void
optCarryCall(OptTableP tableP, OptPreserveP sumsP, int leg, unsigned int mode, OptRoutineP callerP,
             OptWordP callP, OptRoutineP calleeP, const OptCarryState *preP, OptCarryState *postP)
{
OptPreserveP sumP;
const OptWordSet *setP;
int isJda;
int deposit;
int opt;
int oracle;
int keepIo;
int keepAc;
int i;
int at;

    (void)tableP;

    optCarryStateCopy(postP, preP);
    postP->known = 1;
    isJda = ((callP->decode.group == OPTG_MEMREF) && (callP->decode.opcode == PROP_JDA));

    if( isJda )
    {
        deposit = (callP->decode.indirect)?callP->decode.address:PRO_CALSAVE;
        optCarryStateSetFact(postP, deposit, preP->reg[0]);
    }

    // An optimistic run takes every call as harmless, one to the sink too.
    if( mode & OPTCM_CALLSOPT )
    {
        if( !isJda )
        {
            postP->reg[0] = OPTVAL_UNKNOWN;
        }

        return;
    }

    if( !calleeP )
    {
        optCarryStateUnknown(postP);
        return;
    }

    sumP = &sumsP[calleeP->id];
    opt = ((mode & OPTCM_SCCOPT) && callerP && callerP->scc && (callerP->scc == calleeP->scc));
    oracle = ((leg == 3) || (mode & OPTCM_ORACLE));

    if( opt || oracle )
    {
        keepIo = 1;
        keepAc = isJda;
    }
    else
    {
        keepIo = (sumP->done && (((mode & OPTCM_FREESUMS)?sumP->freeIo:sumP->io) == OPTPR_YES));
        keepAc = (isJda && sumP->done && (((mode & OPTCM_FREESUMS)?sumP->freeAc:sumP->ac) == OPTPR_YES));
    }

    if( !keepIo )
    {
        postP->reg[1] = OPTVAL_UNKNOWN;
    }

    if( !keepAc )
    {
        postP->reg[0] = OPTVAL_UNKNOWN;
    }

    // What the callee writes.
    if( opt || (mode & OPTCM_NOWRITES) )
    {
        return;
    }

    if( oracle )
    {
        setP = &sumP->direct;
    }
    else if( !sumP->done )
    {
        postP->factCount = 0;
        return;
    }
    else
    {
        setP = (mode & OPTCM_FREESUMS)?&sumP->freeWrites:&sumP->writes;
    }

    for( i = 0, at = 0; i < postP->factCount; ++i )
    {
        if( optWordSetHas(setP, callP->bank, postP->factsP[i].addr) )
        {
            continue;
        }

        postP->factsP[at++] = postP->factsP[i];
    }

    postP->factCount = at;
}
