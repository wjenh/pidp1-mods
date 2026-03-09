// The command handlers
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/mman.h>

#include "ad1.h"
#include "pdp1inc.h"
#include "helpmsgs.h"
#include "y.tab.h"

int lastAddr;
int base;               // default input number base, changed by the base command
int lastFormat;         // last format used, 0 means use base
int curBank;            // set by the bank cmd
int windowSize = 3;     // 3 lines before and after the current line
int brkCount;           // number of set breakpoints
int watchCount;         // number of set watches

extern char *am1NameP;
extern char *lstNameP;
extern char *symNameP;

void helpFn(char *nameP);
void showFn(int addr, int base, bool noDeref);
void showRegisterFn(int reg, int base);
void setFn(int type, int addr, int value);
void startFn(int addr);
void stopFn(void);
void stepFn(void);
void continueFn(void);
void nextFn(void);
void setBankFn(int num);
void setBpFn(int num, int count);
void deleteBpFn(int num);
void enableBpFn(int num);
void disableBpFn(int num);
void setBaseFn(int num);
void setFileFn(char *nameP, bool add);
void listFn(int lineNo, MapEntryP mapP);
void setWatchFn(int addr,  int value);
void deleteWatchFn(int num);
void enableWatchFn(int num);
void disableWatchFn(int num);
void setWindowFn(int size);
void debugFn(void);

extern PDP1P pdp1P;     // the emulator state in shared memory
extern char *symNameP;
extern Dispatch dispatchTable[];
extern Dispatch extraHelpTable[];
extern BreakpointP isBreakpoint(int addr);

extern MapEntryP getLinesFromAddress(int address);
extern int getCurrentPC(void);
extern bool loadFileData(void);
extern char *getFormat(int fmt);
extern char *getUnrestrictedFormat(int fmt);
extern void formatAndPrintOne(int fmt, int value);
extern void formatAndPrintTwo(int fmt1, int addr, int fmt2,  int value);

extern void listSymbols(void);

extern void listBreaks(void);
extern bool validateBreakpointNumber(int num);
extern void clearBreakpoint(BreakpointP brkP);
extern void deleteAllBreakpoints(void);

extern void listWatches(void);
extern bool validateWatchNumber(int num);
extern void clearWatch(WatchP watchP);
extern void deleteAllWatches(void);

extern int onesCompl(int val);
extern bool loadSymbols(char *filenameP);
extern void clearFiles(void);
extern void clearSymbols(void);
extern void closeListFile(void);
extern void resolveFiles(char *nameP, char **am1PP, char **symPP, char **lstPP);
extern void clearSymbols(void);
extern void closeFile(void);
extern bool printLine(int lineno);
extern bool printLines(MapEntryP linesP);
extern bool printNextLine(void);
extern int getCurrentLineNumber(void);
extern DispatchP findCommand(DispatchP dipatchTable, char *nameP);

void
helpFn(char *nameP)
{
int i;
char *cP;
DispatchP dispP;

    if( nameP != NIL )
    {
        if( (dispP = findCommand(dispatchTable, nameP)) )
        {
            if( dispP->helpText )
            {
                showText(dispP->helpText);
                NEWLINE;
            }
        }
        else if( (dispP = findCommand(extraHelpTable, nameP)) )
        {
            if( dispP->helpText )
            {
                showText(dispP->helpText);
                NEWLINE;
            }
        }
        else
        {
            printf("No help exists for %s\n", nameP);
        }
    }
    else
    {
        showText(helpMsg);
        for( dispP = dispatchTable; dispP->nameP != 0; ++dispP )
        {
            cP = dispP->nameP;
            for( i = 0; i < dispP->significant; ++i )
            {
                fputc(toupper(*cP++), stdout);
            }
            printf("%s\n", cP);
        }

        printf("\nHelp is also available for:\n");

        for( dispP = extraHelpTable; dispP->nameP != 0; ++dispP )
        {
            cP = dispP->nameP;
            for( i = 0; i < dispP->significant; ++i )
            {
                fputc(toupper(*cP++), stdout);
            }
            printf("%s\n", cP);
        }
        NEWLINE;
    }
}

void
nextFn(void)
{
    if( ++lastAddr >= MAXMEM )
    {
        lastAddr = 0;       // just wrap around
    } 

    showFn(lastAddr, lastFormat, false);
}

void
showFn(int addr, int base, bool noDeref)
{
int val;

    lastAddr = addr;

    if( noDeref )
    {
        formatAndPrintOne(base, addr);
        NEWLINE;
    }
    else
    {
        val = (int)pdp1P->core[addr];
        formatAndPrintTwo(ADDRESS, lastAddr, base, val);
        NEWLINE;
    }

    lastFormat = base;
}

void
showRegisterFn(int reg, int base)
{
u32 val;
char *nameP;

    switch( reg )
    {
    case ACREG:
        nameP = "AC";
        val = pdp1P->ac;
        break;
    case IOREG:
        nameP = "IO";
        val = pdp1P->io;
        break;
    case PCREG:
        nameP = "PC";
        val = pdp1P->epc | pdp1P->pc;
        break;
    case TWREG:
        nameP = "Test word switches";
        val = pdp1P->tw;
        break;
    case PFREG:
        printf("Program flags: %06b\n", pdp1P->pf);
        return;
    case SSREG:
        printf("Sense switches: %06b\n", pdp1P->ss);
        return;
    case ASREG:
        nameP = "Address switches";
        val = pdp1P->ta;
        break;
    case MAREG:
        nameP = "MA";
        val = pdp1P->ma;
        break;
    case MBREG:
        nameP = "MB";
        val = pdp1P->mb;
        break;
    case DOT:
        nameP = ".";
        val = pdp1P->core[lastAddr];
        break;
    case SYREG:
        listSymbols();
        return;
    case BREAK:
        listBreaks();
        return;
    case WATCH:
        listWatches();
        return;

    default:
        printf("Internal error, bad register type %d\n", reg);
        return;
    }

    printf("%s: ", nameP);
    formatAndPrintOne(base, val);
    NEWLINE;
    lastFormat = base;
}

void
setFn(int type, int addr, int value)
{
int flag;
bool clr;
BreakpointP brkP;

    if( (brkP = isBreakpoint(addr)) )
    {
        printf("The address cannot be set unless breakpoint %d is deleted first.\n", brkP->number);
    }

    value &= 0777777;           // only 18 bis

    if( type == REGISTER )
    {
        switch( addr )          // is actually the register id
        {
        case PCREG:
            if( value >= MAXMEM )
            {
                printf("The address must be less than %d.\n", MAXMEM);
                return;
            }

            pdp1P->pc = value & 07777;    
            pdp1P->epc = value & 0170000;    
            pdp1P->exd = (pdp1P->epc != 0);

            lastAddr = value;
            break;

        case ACREG:
            pdp1P->ac = onesCompl(value);
            break;

        case IOREG:
            pdp1P->io = onesCompl(value);
            break;

        case PFREG:
            if( value == 0 )
            {
                pdp1P->pf = 0;
            }
            else if( value > 0x3F )
            {
                printf("The value must be between 1 and 077, hex 3F, decimal 63.\n");
                printf("The bit pattern determines which flags are set.\n");
            }
            else
            {
                pdp1P->pf = value;
            }
            break;

        case PF1REG:
        case PF2REG:
        case PF3REG:
        case PF4REG:
        case PF5REG:
        case PF6REG:
            if( (value < 0) || (value > 1) )
            {
                printf("The value must be 0 to clear or 1 to set.\n");
            }

            if( value == 0 )
            {
                // means clear it
                clr = 1;
            }
            else
            {
                clr = 0;
            }

            flag = 5 - (addr - PF1REG);
            if( clr )
            {
                pdp1P->pf &= ~(1 << flag) & 0x3F;
            }
            else
            {
                pdp1P->pf |= 1 << flag;
            }
            break;

        default:
            printf("That register cannot be set.\n");
            break;
        }
    }
    else
    {
        pdp1P->core[addr] = value;
        lastAddr = addr;
    }
}

void
startFn(int addr)
{
    pdp1P->run_enable = 0;      // stop it
    pdp1P->ad1StartAddr = addr & 07777;    
    pdp1P->ad1ExtendedAddr = addr & 0170000;    
    AD1_SET_START(pdp1P);
    lastAddr = addr;
}

void
stopFn(void)
{
    pdp1P->run_enable = 0;
    lastAddr = pdp1P->epc | pdp1P->pc;
}

void
stepFn(void)
{
MapEntryP entryP;

    if( pdp1P->run )
    {
        printf("Must be stopped to step.\n");
    }
    else
    {
        AD1_SET_STEP(pdp1P);
        pdp1P->run = 1;
        usleep(1000);            // plenty of time for completion

        if( (entryP = getLinesFromAddress(getCurrentPC())) > 0 )
        {
            printLines(entryP);
        }
    }
}

void
continueFn(void)
{
    AD1_SET_CONTINUE(pdp1P);
}

void
setBankFn(int bankno)
{
    if( (bankno < 0) || (bankno >= MAXBANKS) )
    {
        printf("A bank number must be 0-%d decimal or the hex or octal equivalent.\n", MAXBANKS - 1);
    }

    curBank = bankno;
}

void
setBpFn(int addr, int count)
{
int i;
BreakpointP brkP;

    if( count == BADNUM )
    {
        count = 1;          // no count was given, default to 1
    }

    // Find an empty slot
    brkP = pdp1P->ad1Breakpoints;

    for( i = 0; i < AD1_NUM_BREAKPOINTS; ++i )
    {
        if( !brkP->isSet )
        {
            break;
        }

        ++brkP;
    }

    if( i >= AD1_NUM_BREAKPOINTS )
    {
        printf("No breakpoints are left, delete one first\n");
        return;
    }

    brkP->isSet = true;
    brkP->number = i + 1;
    brkP->address = addr;
    brkP->count = count;
    brkP->curCount = 0;
    brkP->isEnabled = true;

    AD1_ENABLE_BREAKPOINTS(pdp1P);
    ++brkCount;
}

// 0 means all of them
void
deleteBpFn(int bpno)
{
BreakpointP brkP;
char line[32];

    if( !validateBreakpointNumber((bpno)?bpno:1) )
    {
        return;
    }

    if( bpno < 1 )
    {
        // delete them all?
        printf("Delete all breakpints? [y to delete] ");
        fgets(line, sizeof(line), stdin);
        if( *line != 'y' )
        {
            return;
        }

        deleteAllBreakpoints();
    }
    else
    {
        brkP = &(pdp1P->ad1Breakpoints[bpno - 1]);
        if( !brkP->isSet )
        {
            printf("Breakpoint %d is not set.\n", bpno);
        }
        else
        {
            clearBreakpoint(brkP);
        }
    }
}

void
enableBpFn(int bpno)
{
BreakpointP brkP;

    if( !validateBreakpointNumber(bpno) )
    {
        return;
    }

    brkP = &(pdp1P->ad1Breakpoints[bpno-1]);
    if( brkP->isSet )
    {
        if( brkP->isEnabled )
        {
            printf("Breakpoint %d is already enabled.\n", brkP->number);
        }
        else
        {
            brkP->isEnabled = true;
        }
    }
    else
    {
        printf("Breakpoint %d is not set, can't enable it.\n", bpno);
    }
}

void
disableBpFn(int bpno)
{
BreakpointP brkP;

    if( !validateBreakpointNumber(bpno) )
    {
        return;
    }

    brkP = &(pdp1P->ad1Breakpoints[bpno - 1]);
    if( brkP->isSet )
    {
        if( !brkP->isEnabled )
        {
            printf("Breakpoint %d is already diabled.\n", brkP->number);
        }
        else
        {
            brkP->isEnabled = false;
        }
    }
    else
    {
        printf("Breakpoint %d is not set, can't disable it.\n", bpno);
    }
}

void
setBaseFn(int num)
{
    switch( num )
    {
    case 2:
    case 8:
    case 10:
    case 16:
        lastFormat = base = num;
        break;

    default:
        printf("Base must be beteeen 1 and 32\n");
    }
}

// User gave a file name, clear any open and use it unless add is true.
// In that case only try for a symbol file and add those to the existing symbol table.
void
setFileFn(char *nameP, bool add)
{
char *tmpP;

    if( add )
    {
        resolveFiles(nameP, NIL, &tmpP, NIL);
        loadSymbols(tmpP);
        free(tmpP);
    }
    else
    {
        clearFiles();
        closeListFile();
        clearSymbols();
        resolveFiles(nameP, &am1NameP, &symNameP, &lstNameP);
        loadFileData();             // before loading symbols!
        loadSymbols(symNameP);
    }
}

// List can be called with NOARG, NIL which means continue listing from one past the last line,
// value, NIL which means list from the value line, or
// NOARG, mapP which means list all lines associated with the map entry.
void
listFn(int lineNo, MapEntryP mapP)
{
int i;

    if( !loadFileData() )                   // try to initialize if needed
    {
        return;
    }

    if( (lineNo == NOARG) && !mapP )
    {
        lineNo = getCurrentLineNumber();
    }
    else if( mapP )
    {
        printLines(mapP);
        return;
    }

    if( (lineNo -= windowSize) < 1 )
    {
        lineNo = 1;
    }

    for( i = 0; i < ((windowSize * 2) + 1); ++i )
    {
        if( lineNo == getCurrentLineNumber() )
        {
            if( !printNextLine() )
            {
                printf("eof\n");
                return;
            }
        }
        else
        {
            if( !printLine(lineNo) )
            {
                printf("eof\n");
                return;
            }
        }

        ++lineNo;
    }
}

void
setWatchFn(int addr, int value)
{
int i;
int flags;
WatchP watchP;

    // Find an empty slot
    watchP = pdp1P->ad1Watches;

    for( i = 0; i < AD1_NUM_WATCHES; ++i )
    {
        if( !watchP->isSet )
        {
            break;
        }

        ++watchP;
    }

    if( i >= AD1_NUM_WATCHES )
    {
        printf("No watches are left, delete one first\n");
        return;
    }

    watchP->isSet = true;
    watchP->number = i + 1;
    watchP->address = addr & 0177777;
    watchP->lastVal = pdp1P->core[watchP->address];

    if( value == BADNUM )
    {
        watchP->onAny = true;
        watchP->value = 0;      // just to keep it clean
    }
    else
    {
        watchP->onAny = false;
        watchP->value = value;
    }

    watchP->isEnabled = true;
    AD1_ENABLE_WATCHES(pdp1P);

    ++watchCount;
}

void
deleteWatchFn(int num)
{
WatchP watchP;
char line[32];

    if( num < 1 )
    {
        // delete them all?
        printf("Delete all watches? [y to delete] ");
        fgets(line, sizeof(line), stdin);
        if( *line != 'y' )
        {
            return;
        }

        deleteAllWatches();
    }
    else
    {
        if( !validateWatchNumber(num) )
        {
            return;
        }

        watchP = &(pdp1P->ad1Watches[num - 1]);
        if( !watchP->isSet )
        {
            printf("Watch %d is not set.\n", num);
        }
        else
        {
            clearWatch(watchP);

            if( --watchCount <= 0 )
            {
                AD1_DISABLE_WATCHES(pdp1P);
            }
        }
    }
}

void
enableWatchFn(int num)
{
WatchP watchP;

    if( !validateWatchNumber(num) )
    {
        return;
    }

    watchP = &(pdp1P->ad1Watches[num-1]);
    if( watchP->isSet )
    {
        if( watchP->isEnabled )
        {
            printf("Watch %d is already enabled.\n", watchP->number);
        }
        else
        {
            watchP->isEnabled = true;
            watchP->lastVal = pdp1P->core[watchP->address]; // update so we don't fire until the next change
        }
    }
    else
    {
        printf("Watch %d is not set, can't enable it.\n", num);
    }
}

void
disableWatchFn(int num)
{
WatchP watchP;

    if( !validateWatchNumber(num) )
    {
        return;
    }

    watchP = &(pdp1P->ad1Watches[num - 1]);
    if( watchP->isSet )
    {
        if( !watchP->isEnabled )
        {
            printf("Watch %d is already diabled.\n", watchP->number);
        }
        else
        {
            watchP->isEnabled = false;
        }
    }
    else
    {
        printf("Watch %d is not set, can't disable it.\n", num);
    }
}

void
setWindowFn(int size)
{
    if( size < 0 )
    {
        printf("Window size must be > 0\n");
    }
    else
    {
        windowSize = size;
    }
}

// Print out various internal things.
void
debugFn()
{
int i;
int val;
BreakpointP brkP;
WatchP watchP;

    printf("ad1flags %x, pc %06o epc %o exd %d\n", pdp1P->ad1flags, pdp1P->pc, pdp1P->epc, pdp1P->exd);
    printf("bps %s ad1brkno %d ad1brkhit %d\n",
         AD1_BREAKPOINTS_ENABLED(pdp1P)?"on":"off",
         pdp1P->ad1brkNo, pdp1P->ad1brkHit);
    printf("watches %s ad1watchno %d ad1watchhit %d\n",
         AD1_WATCHES_ENABLED(pdp1P)?"on":"off",
         pdp1P->ad1watchNo, pdp1P->ad1watchHit);

    for(i = 0; i < AD1_NUM_BREAKPOINTS; ++i )
    {
        brkP = &pdp1P->ad1Breakpoints[i];
        printf("bkp %d set %d enabled %d address %06o\n", i+1, brkP->isSet, brkP->isEnabled, brkP->address);
    }

    for(i = 0; i < AD1_NUM_WATCHES; ++i )
    {
        watchP = &pdp1P->ad1Watches[i];
        printf("watch %d set %d enabled %d address %06o,",
            i+1, watchP->isSet, watchP->isEnabled, watchP->address);

        val = pdp1P->core[watchP->address];

        if( watchP->onAny )
        {
            printf(" value any, value %06o, lastval %06o, memval %06o\n",
                watchP->value, watchP->lastVal, val);
        }
        else
        {
            printf(" value %06o, lastVal %06o, memval %06o\n", watchP->value, watchP->lastVal, val);
        }
    }
}

