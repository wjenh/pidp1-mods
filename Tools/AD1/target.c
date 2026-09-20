// What ad1 talks to, the emulator over the debugger link, or in test mode a local image.
//
// 20-Sep-2026 Claude: written to replace direct access to the emulator's shared memory.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "target.h"

static bool isLocal;
static bool isDead;                 // the link is gone and the program is on its way out
static bool linkIsOpen;
static Ad1Link lnk;

// The test-mode image. There is no processor, so run control only records what was asked for.
static struct
{
    uint32_t core[AD1P_MEM_WORDS];
    uint32_t ac;
    uint32_t io;
    uint32_t pc;
    uint32_t pf;
    uint32_t run;
    uint32_t single;
    Ad1BpEntry bp[AD1P_NUM_BREAKPOINTS];
    Ad1WatchEntry watch[AD1P_NUM_WATCHES];
} local;

// A lost connection ends the program. Anything else is the caller's to handle.
static int
check(int status)
{
    if( status == AD1L_COMM )
    {
        fprintf(stderr, "\n%s\n", ad1LinkError());
        isDead = true;
        linkIsOpen = false;
        exit(1);
    }

    return( status );
}

#define IS_GONE() (isDead || (!isLocal && !linkIsOpen))

int
tgtOpen(const char *hostSpec)
{
    if( ad1LinkOpen(&lnk, hostSpec, 0, "ad1") != 0 )
    {
        return( -1 );
    }

    linkIsOpen = true;
    return( 0 );
}

void
tgtOpenLocal(void)
{
    isLocal = true;
    memset(&local, 0, sizeof(local));
    local.pf = 0;
}

uint32_t *
tgtLocalCore(void)
{
    return( local.core );
}

bool
tgtIsLocal(void)
{
    return( isLocal );
}

void
tgtClose(int policy)
{
    if( isLocal || !linkIsOpen || isDead )
    {
        return;
    }

    // Whatever happens to the client after this, the emulator applies the policy last given.
    ad1SetPolicy(&lnk, (uint32_t)policy);
    ad1LinkClose(&lnk);
    linkIsOpen = false;
}

int
tgtFd(void)
{
    return( (isLocal || !linkIsOpen) ? -1 : ad1LinkFd(&lnk) );
}

int
tgtPump(void)
{
    if( IS_GONE() || isLocal )
    {
        return( 0 );
    }

    if( ad1LinkPump(&lnk) != 0 )
    {
        check(AD1L_COMM);
    }

    return( lnk.evCount > 0 );
}

int
tgtNextEvent(Ad1Event *evP)
{
    if( isLocal || !linkIsOpen )
    {
        return( 0 );
    }

    return( ad1LinkNextEvent(&lnk, evP) );
}

uint32_t
tgtRead(uint32_t address)
{
uint32_t word;

    if( address >= AD1P_MEM_WORDS )
    {
        return( 0 );
    }

    if( isLocal )
    {
        return( local.core[address] );
    }

    if( IS_GONE() || (check(ad1ReadMem(&lnk, address, 1, &word)) != AD1P_ST_OK) )
    {
        return( 0 );
    }

    return( word );
}

int
tgtWrite(uint32_t address, uint32_t value)
{
    if( address >= AD1P_MEM_WORDS )
    {
        return( AD1P_ST_BAD_ARG );
    }

    if( isLocal )
    {
        local.core[address] = (value & 0777777);
        return( AD1P_ST_OK );
    }

    if( IS_GONE() )
    {
        return( AD1P_ST_BAD_STATE );
    }

    return( check(ad1WriteMem(&lnk, address, 1, &value)) );
}

int
tgtWriteBlocks(const Ad1Block *blocksP, uint32_t nBlocks, const uint32_t *wordsP, bool stopFirst,
    bool *wasRunningP)
{
uint32_t wasRunning;
uint32_t i;
uint32_t j;
uint32_t used;
int status;

    *wasRunningP = false;
    if( isLocal )
    {
        used = 0;
        for( i = 0; i < nBlocks; ++i )
        {
            if( (blocksP[i].address >= AD1P_MEM_WORDS) || (blocksP[i].count > (AD1P_MEM_WORDS - blocksP[i].address)) )
            {
                return( AD1P_ST_BAD_ARG );
            }
        }

        for( i = 0; i < nBlocks; ++i )
        {
            for( j = 0; j < blocksP[i].count; ++j )
            {
                local.core[blocksP[i].address + j] = (wordsP[used++] & 0777777);
            }
        }

        return( AD1P_ST_OK );
    }

    if( IS_GONE() )
    {
        return( AD1P_ST_BAD_STATE );
    }

    wasRunning = 0;
    status = check(ad1WriteBlocks(&lnk, blocksP, nBlocks, wordsP, stopFirst, &wasRunning, NULL));
    *wasRunningP = (wasRunning != 0);
    return( status );
}

int
tgtGetState(uint32_t *state)
{
    if( isLocal )
    {
        memset(state, 0, AD1P_STATE_WORDS * sizeof(uint32_t));
        state[AD1P_STATE_AC] = local.ac;
        state[AD1P_STATE_IO] = local.io;
        state[AD1P_STATE_PC] = local.pc;
        state[AD1P_STATE_PF] = local.pf;
        state[AD1P_STATE_EXD] = (local.pc > 07777);
        state[AD1P_STATE_RUN] = local.run;
        state[AD1P_STATE_POWER] = 1;
        state[AD1P_STATE_SINGLE] = local.single;
        state[AD1P_STATE_WORD_AT_PC] = local.core[local.pc & (AD1P_MEM_WORDS - 1)];
        return( AD1P_ST_OK );
    }

    if( IS_GONE() )
    {
        memset(state, 0, AD1P_STATE_WORDS * sizeof(uint32_t));
        return( AD1P_ST_BAD_STATE );
    }

    return( check(ad1GetState(&lnk, state)) );
}

int
tgtSetReg(uint32_t reg, uint32_t op, uint32_t value)
{
    if( isLocal )
    {
        if( (op != AD1P_OP_ASSIGN) && (reg != AD1P_REG_PF) )
        {
            return( AD1P_ST_BAD_ARG );
        }

        switch( reg )
        {
        case AD1P_REG_AC:
            local.ac = (value & 0777777);
            return( AD1P_ST_OK );

        case AD1P_REG_IO:
            local.io = (value & 0777777);
            return( AD1P_ST_OK );

        case AD1P_REG_PC:
            if( value >= AD1P_MEM_WORDS )
            {
                return( AD1P_ST_BAD_ARG );
            }

            local.pc = value;
            return( AD1P_ST_OK );

        case AD1P_REG_PF:
            if( value > 077 )
            {
                return( AD1P_ST_BAD_ARG );
            }

            if( op == AD1P_OP_ASSIGN )
            {
                local.pf = value;
            }
            else if( op == AD1P_OP_OR )
            {
                local.pf |= value;
            }
            else
            {
                local.pf &= (~value & 077);
            }

            return( AD1P_ST_OK );

        default:
            return( AD1P_ST_BAD_ARG );
        }
    }

    if( IS_GONE() )
    {
        return( AD1P_ST_BAD_STATE );
    }

    return( check(ad1SetReg(&lnk, reg, op, value)) );
}

int
tgtStart(uint32_t address)
{
    if( isLocal )
    {
        local.pc = address;
        local.run = 1;
        local.single = 0;
        return( AD1P_ST_OK );
    }

    if( IS_GONE() )
    {
        return( AD1P_ST_BAD_STATE );
    }

    return( check(ad1Start(&lnk, address, NULL, NULL)) );
}

int
tgtStop(uint32_t *pcP)
{
uint32_t run;

    if( isLocal )
    {
        local.run = 0;
        local.single = 0;
        *pcP = local.pc;
        return( AD1P_ST_OK );
    }

    if( IS_GONE() )
    {
        return( AD1P_ST_BAD_STATE );
    }

    return( check(ad1Stop(&lnk, &run, pcP)) );
}

int
tgtContinue(void)
{
    if( isLocal )
    {
        local.run = 1;
        local.single = 0;
        return( AD1P_ST_OK );
    }

    if( IS_GONE() )
    {
        return( AD1P_ST_BAD_STATE );
    }

    return( check(ad1Continue(&lnk, NULL)) );
}

int
tgtStep(uint32_t count, bool record, Ad1StepResult *resP)
{
    if( isLocal )
    {
        return( AD1P_ST_BAD_STATE );            // nothing here executes instructions
    }

    if( IS_GONE() )
    {
        return( AD1P_ST_BAD_STATE );
    }

    return( check(ad1Step(&lnk, count, record, resP)) );
}

int
tgtClearSingle(void)
{
    if( isLocal )
    {
        local.single = 0;
        return( AD1P_ST_OK );
    }

    if( IS_GONE() )
    {
        return( AD1P_ST_BAD_STATE );
    }

    return( check(ad1ClearSingle(&lnk)) );
}

// The local breakpoint and watch tables follow the server's rules: it picks the first free
// entry, and says NOT_SET, ALREADY or NO_SLOT for the same cases.
int
tgtBpSet(uint32_t address, uint32_t count, uint32_t *numberP)
{
int i;

    if( isLocal )
    {
        for( i = 0; i < AD1P_NUM_BREAKPOINTS; ++i )
        {
            if( !local.bp[i].isSet )
            {
                local.bp[i].isSet = 1;
                local.bp[i].isEnabled = 1;
                local.bp[i].number = (uint32_t)(i + 1);
                local.bp[i].address = (address & 0177777);
                local.bp[i].count = count;
                local.bp[i].curCount = 0;
                *numberP = (uint32_t)(i + 1);
                return( AD1P_ST_OK );
            }
        }

        return( AD1P_ST_NO_SLOT );
    }

    if( IS_GONE() )
    {
        return( AD1P_ST_BAD_STATE );
    }

    return( check(ad1BpSet(&lnk, address, count, numberP)) );
}

int
tgtBpDelete(uint32_t number)
{
    if( isLocal )
    {
        if( number > AD1P_NUM_BREAKPOINTS )
        {
            return( AD1P_ST_BAD_ARG );
        }

        if( number == 0 )
        {
            memset(local.bp, 0, sizeof(local.bp));
            return( AD1P_ST_OK );
        }

        if( !local.bp[number - 1].isSet )
        {
            return( AD1P_ST_NOT_SET );
        }

        memset(&local.bp[number - 1], 0, sizeof(local.bp[0]));
        return( AD1P_ST_OK );
    }

    if( IS_GONE() )
    {
        return( AD1P_ST_BAD_STATE );
    }

    return( check(ad1BpDelete(&lnk, number)) );
}

// Enable or disable a local breakpoint or watch, given whether it is set, enabled, and what to
// do. Returns the status the server would give.
static int
localToggle(int isSet, int *enabledP, int enable)
{
    if( !isSet )
    {
        return( AD1P_ST_NOT_SET );
    }

    if( (*enabledP != 0) == (enable != 0) )
    {
        return( AD1P_ST_ALREADY );
    }

    *enabledP = enable;
    return( AD1P_ST_OK );
}

int
tgtBpEnable(uint32_t number)
{
    if( isLocal )
    {
        if( (number < 1) || (number > AD1P_NUM_BREAKPOINTS) )
        {
            return( AD1P_ST_BAD_ARG );
        }

        return( localToggle(local.bp[number - 1].isSet, &local.bp[number - 1].isEnabled, 1) );
    }

    if( IS_GONE() )
    {
        return( AD1P_ST_BAD_STATE );
    }

    return( check(ad1BpEnable(&lnk, number)) );
}

int
tgtBpDisable(uint32_t number)
{
    if( isLocal )
    {
        if( (number < 1) || (number > AD1P_NUM_BREAKPOINTS) )
        {
            return( AD1P_ST_BAD_ARG );
        }

        return( localToggle(local.bp[number - 1].isSet, &local.bp[number - 1].isEnabled, 0) );
    }

    if( IS_GONE() )
    {
        return( AD1P_ST_BAD_STATE );
    }

    return( check(ad1BpDisable(&lnk, number)) );
}

int
tgtBpList(Ad1BpEntry *entriesP)
{
uint32_t n;
int status;

    if( isLocal )
    {
        memcpy(entriesP, local.bp, sizeof(local.bp));
        return( AD1P_ST_OK );
    }

    if( IS_GONE() )
    {
        memset(entriesP, 0, AD1P_NUM_BREAKPOINTS * sizeof(*entriesP));
        return( AD1P_ST_BAD_STATE );
    }

    memset(entriesP, 0, AD1P_NUM_BREAKPOINTS * sizeof(*entriesP));
    status = check(ad1BpList(&lnk, entriesP, AD1P_NUM_BREAKPOINTS, &n));
    return( status );
}

int
tgtWatchSet(uint32_t address, bool onAnyChange, uint32_t value, uint32_t *numberP)
{
int i;

    if( isLocal )
    {
        for( i = 0; i < AD1P_NUM_WATCHES; ++i )
        {
            if( !local.watch[i].isSet )
            {
                local.watch[i].isSet = 1;
                local.watch[i].isEnabled = 1;
                local.watch[i].number = (uint32_t)(i + 1);
                local.watch[i].address = (address & 0177777);
                local.watch[i].onAnyChange = onAnyChange ? 1 : 0;
                local.watch[i].value = onAnyChange ? 0 : value;
                local.watch[i].lastValue = local.core[local.watch[i].address];
                *numberP = (uint32_t)(i + 1);
                return( AD1P_ST_OK );
            }
        }

        return( AD1P_ST_NO_SLOT );
    }

    if( IS_GONE() )
    {
        return( AD1P_ST_BAD_STATE );
    }

    return( check(ad1WatchSet(&lnk, address, onAnyChange, value, numberP)) );
}

int
tgtWatchDelete(uint32_t number)
{
    if( isLocal )
    {
        if( number > AD1P_NUM_WATCHES )
        {
            return( AD1P_ST_BAD_ARG );
        }

        if( number == 0 )
        {
            memset(local.watch, 0, sizeof(local.watch));
            return( AD1P_ST_OK );
        }

        if( !local.watch[number - 1].isSet )
        {
            return( AD1P_ST_NOT_SET );
        }

        memset(&local.watch[number - 1], 0, sizeof(local.watch[0]));
        return( AD1P_ST_OK );
    }

    if( IS_GONE() )
    {
        return( AD1P_ST_BAD_STATE );
    }

    return( check(ad1WatchDelete(&lnk, number)) );
}

int
tgtWatchEnable(uint32_t number)
{
int status;

    if( isLocal )
    {
        if( (number < 1) || (number > AD1P_NUM_WATCHES) )
        {
            return( AD1P_ST_BAD_ARG );
        }

        status = localToggle(local.watch[number - 1].isSet, &local.watch[number - 1].isEnabled, 1);
        if( status == AD1P_ST_OK )
        {
            local.watch[number - 1].lastValue = local.core[local.watch[number - 1].address];
        }

        return( status );
    }

    if( IS_GONE() )
    {
        return( AD1P_ST_BAD_STATE );
    }

    return( check(ad1WatchEnable(&lnk, number)) );
}

int
tgtWatchDisable(uint32_t number)
{
    if( isLocal )
    {
        if( (number < 1) || (number > AD1P_NUM_WATCHES) )
        {
            return( AD1P_ST_BAD_ARG );
        }

        return( localToggle(local.watch[number - 1].isSet, &local.watch[number - 1].isEnabled, 0) );
    }

    if( IS_GONE() )
    {
        return( AD1P_ST_BAD_STATE );
    }

    return( check(ad1WatchDisable(&lnk, number)) );
}

int
tgtWatchList(Ad1WatchEntry *entriesP)
{
uint32_t n;

    if( isLocal )
    {
        memcpy(entriesP, local.watch, sizeof(local.watch));
        return( AD1P_ST_OK );
    }

    memset(entriesP, 0, AD1P_NUM_WATCHES * sizeof(*entriesP));
    if( IS_GONE() )
    {
        return( AD1P_ST_BAD_STATE );
    }

    return( check(ad1WatchList(&lnk, entriesP, AD1P_NUM_WATCHES, &n)) );
}

int
tgtAckHit(uint32_t mask)
{
    if( isLocal || IS_GONE() )
    {
        return( AD1P_ST_OK );
    }

    return( check(ad1AckHit(&lnk, mask)) );
}

int
tgtSetPolicy(uint32_t policy)
{
    if( isLocal || IS_GONE() )
    {
        return( AD1P_ST_OK );
    }

    return( check(ad1SetPolicy(&lnk, policy)) );
}

int
tgtDisableAll(void)
{
int i;

    if( isLocal )
    {
        for( i = 0; i < AD1P_NUM_BREAKPOINTS; ++i )
        {
            local.bp[i].isEnabled = 0;
        }

        for( i = 0; i < AD1P_NUM_WATCHES; ++i )
        {
            local.watch[i].isEnabled = 0;
        }

        return( AD1P_ST_OK );
    }

    if( IS_GONE() )
    {
        return( AD1P_ST_BAD_STATE );
    }

    return( check(ad1DisableAll(&lnk)) );
}
