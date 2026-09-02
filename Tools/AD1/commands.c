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
#include "../Disassembler/decode_instruction.h"

int lastAddr;
int base;               // default input number base, changed by the base command
int lastFormat;         // last format used, 0 means use base
int curStartAddr;       // set by the start or load commands
int curBank;            // set by the bank cmd
int windowSize = 6;     // default window size
int brkCount;           // number of set breakpoints
int watchCount;         // number of set watches

extern int numFiles;
extern int curFileNo;
extern FileInfoP files[MAXFILES];

void helpFn(char *nameP);
void showFn(int addr, int base, bool noDeref);
void showRegisterFn(int reg, int base);
void setFn(int type, int addr, int value);
void startFn(int addr);
void stopFn(void);
void stepFn(int count);
void continueFn(void);
void nextFn(void);
void formatFn(int base);
void setBankFn(int num);
void setBpFn(int num, int count);
void deleteBpFn(int num);
void enableBpFn(int num);
void disableBpFn(int num);
void setBaseFn(int num);
void setFileFn(char *nameP, bool add);
void listFn(int lineNo, int fileNo);
void traceFn(int addr);
void loadFn(char *filenameP);
void monitorFn(int count, char *nameP);
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

extern int getMapForFileNo(MapEntryP mapP, int fileNo);
extern MapEntryP getLinesFromAddress(int address);
extern int getCurrentPC(void);
extern char *getFormat(int fmt);
extern char *findNameByAddr(int addr);
extern char *getFormatName(int fmt);
extern char *getUnrestrictedFormat(int fmt);
extern void formatAndPrintOne(int fmt, int value);
extern void formatAndPrintTwo(int fmt1, int addr, int fmt2,  int value);
extern char *decodeInstr(int word, int addr, bool asMacro, char *separatorP, char *symbolP,
    char *resultP, CodeDefP defP);

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
extern bool isFileMapped(int fileno);
extern FileInfoP newFile(char *nameP);
extern FileInfoP getFileInfoP(int fileNo);
extern bool openSourceFile(FileInfoP infoP);

extern void closeFiles(void);
extern bool printLine(int fileno, int lineno);
extern bool printLines(MapEntryP linesP);
extern bool printNextLine(void);
extern int getCurrentLineNumber(void);
extern void setCurrentLineNumber(int lineNo);
extern int loadTape(char *filenameP);
extern bool findRimFile(FileInfoP infoP, char *rsltP);
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
formatFn(int base)
{
    if( base == NONE )      // special marker, show the current base
    {
        printf("Curremt format is %s\n", getFormatName(lastFormat));
    }
    else
    {
        lastFormat = base;
    }
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
        formatAndPrintTwo(ADDRESS, addr, base, val);
        NEWLINE;
    }
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
}

void
setFn(int type, int addr, int value)
{
int flag;
bool clr;
BreakpointP brkP;

    if( (type != REGISTER) && (brkP = isBreakpoint(addr)) )
    {
        printf("The address cannot be set unless breakpoint %d is deleted first.\n", brkP->number);
        return;
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

            pdp1P->pc = ADDRESSOF(value);
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
    if( addr < 0 )
    {
        printf("No start address has been set, give one or load a tape.\n");
    }
    else
    {
        pdp1P->run_enable = 0;      // stop it
        pdp1P->ad1StartAddr = ADDRESSOF(addr);
        pdp1P->ad1ExtendedAddr = addr & 0170000;    
        AD1_CLEAR_SINGLE(pdp1P);    // shouldn't be set, but be sure
        AD1_SET_START(pdp1P);
        curStartAddr = lastAddr = addr;
    }
}

void
stopFn(void)
{
    AD1_CLEAR_SINGLE(pdp1P);    // shouldn't be set, but be sure
    AD1_SET_STOP(pdp1P);
    usleep(1000);       // plenty of time for the stop to happen, we need the pc at that point
    lastAddr = pdp1P->epc | pdp1P->pc;
}

// If count is the number of instruction cycles to step.
// A breakpoint or watchpoint hit will end stepping immediately.
void
stepFn(int count)
{
MapEntryP entryP;

    if( pdp1P->run )
    {
        printf("Must be stopped to step.\n");
    }
    else
    {
        if( count == BADNUM )
        {
            count = 1;
        }

        AD1_SET_SINGLE(pdp1P);   // this is a 'sticky' setting and must be cleared to get out of ss

        while( count-- > 0 )
        {
            AD1_SET_CONTINUE(pdp1P);
            while( AD1_CONTINUE(pdp1P) )
            {
                usleep(1);       // wait for completion
            }

            if( AD1_BREAKPOINT_HIT(pdp1P) || AD1_WATCH_HIT(pdp1P) )
            {
                return;             // stop now, main loop will detect this
            }
        }

        if( (entryP = getLinesFromAddress(getCurrentPC())) > 0 )
        {
            if( !getMapForFileNo(entryP, curFileNo) )
            {
                // Just use the first one.
                curFileNo = entryP->fileNo;
            }

            printLines(entryP);
        }
    }
}

void
continueFn(void)
{
    AD1_CLEAR_SINGLE(pdp1P);
    AD1_SET_CONTINUE(pdp1P);
}

void
setBankFn(int bankno)
{
    if( (bankno < 0) || (bankno >= MEMBANKS) )
    {
        printf("A bank number must be 0-%d decimal or the hex or octal equivalent.\n", MEMBANKS - 1);
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

// User gave a file name, clear any that are open and use it unless add is true.
// In that case, add the file to the current file list.
// If the name is nil or nul, list the current files.
void
setFileFn(char *nameP, bool add)
{
int i;
char *cP;
FileInfoP infoP;
char line[128];

    if( !nameP || !*nameP )
    {
        if( !numFiles )
        {
            printf("No files are open.\n");
        }
        else
        {
            printf("Curreint file is %d\n", curFileNo + 1);
            for( int i = 0; i < numFiles; ++i )
            {
                if( isFileMapped(i) )
                {
                    infoP = files[i];
                    cP = infoP->am1NameP;
                    if( !cP )
                    {
                        cP = infoP->lstNameP;
                    }
                    if( !cP )
                    {
                        cP = infoP->symNameP;
                    }

                    if( cP )
                    {
                        printf("%d - '%s'\n", i+1, cP);
                    }
                }
            }
        }

        return;
    }

    if( isdigit(*nameP) )      // could be a file number
    {
        i = strtol(nameP, &cP, 10);
        if( !cP || !*cP )
        {
            if( isFileMapped(i - 1) )
            {
                curFileNo = i - 1;
            }
            else
            {
                printf("%d is not an open file.\n", i);
            }

            return;
        }
    }

    if( add )
    {
        newFile(nameP);
    }
    else
    {
        // close them all?
        printf("Close all files, removing source and symbol information? [y to close] ");
        fgets(line, sizeof(line), stdin);
        if( *line != 'y' )
        {
            return;
        }

        closeFiles();
        newFile(nameP);
    }
}

// List can be called with lineNo of NOARG which means continue listing from one past the last line,
// A lineNo, a number which means list from that line.
// If lineNo is not NOARG and fileNo is not NOARG, it means line in that file.
void
listFn(int lineNo, int fileNo)
{
int i;

    if( (fileNo == NOARG) && (curFileNo < 0) )
    {
        printf("No source or listing file is available.\n");
        return;
    }

    if( fileNo == NOARG )
    {
        fileNo = curFileNo;
    }

    if( lineNo == NOARG )
    {
        if( curFileNo != fileNo )
        {
            setCurrentLineNumber(1);
        }

        lineNo = getCurrentLineNumber();
    }

    if( (lineNo -= windowSize) < 1 )
    {
        lineNo = 1;
    }

    for( i = 0; i < ((windowSize * 2) + 1); ++i )
    {
        if( (fileNo == curFileNo) && (lineNo == getCurrentLineNumber()) )
        {
            if( !printNextLine() )
            {
                printf("eof\n");
                return;
            }
        }
        else
        {
            if( fileNo != NOARG )
            {
                curFileNo = fileNo;
            }

            if( !printLine(curFileNo, lineNo) )
            {
                printf("eof\n");
                return;
            }
        }

        ++lineNo;
    }
}

void
traceFn(int addr)
{
int word;
int bank;
int tmpaddr;
bool deref;
char *cP;
CodeDef codeDef;
CodeDefP defP;
char instr[128];
char tmpstr[128];

    deref = false;
    if( addr == NOARG )
    {
        addr = lastAddr;
    }

    tmpaddr = addr;             // used if the instruction doesn't indirect
    word = pdp1P->core[addr];

    // We need to decode to get the flags.
    bank = curBank;
    addr = FULLADDR(bank, word);
    defP = &codeDef;
    decodeInstr(word, addr, false, " ", findNameByAddr(addr), instr, defP);

    if( defP->flags & (INSTR_READS | INSTR_WRITES) )
    {
        if( defP->flags & INSTR_INDIRECT )
        {
            printf("%s", instr);
            printf(" indirects to ");
            addr = pdp1P->core[addr];
            formatAndPrintOne(SYMBOLIC, FULLADDR(bank, addr));
            NEWLINE;

            // The behavior of indirect depends upon whether or not eem is in effect.
            // If it is, there is no subsequent indirection.
            if( pdp1P->exd )
            {
                bank = BANKOF(addr);
            }
            else
            {
                deref = true;
            }
        }

        // Follow the indirection chain if there is one.
        // This will never change banks.
        if( deref )
        {
            while( addr & INDIRECT_BIT )
            {
                addr &= ~INDIRECT_BIT;
                tmpaddr = pdp1P->core[FULLADDR(bank, addr)];
                printf("The value at address ");
                formatAndPrintOne(SYMBOLIC, addr);
                printf(" is an indirect to ");
                formatAndPrintOne(SYMBOLIC, tmpaddr & ~INDIRECT_BIT);
                addr = tmpaddr;
                printf(", follow it, y for yes? ");
                fgets(tmpstr, sizeof(tmpstr), stdin);
                if( tmpstr[0] != 'y' )
                {
                    break;
                }
           }
        }

        printf("The target address is ");
        formatAndPrintOne(SYMBOLIC, addr);
        NEWLINE;

        if( defP->flags & (INSTR_JUMPS | INSTR_CALLS | INSTR_JDA) )
        {
            cP = strchr(instr, ' ');
            *cP = '\0';
            printf("The instruction was %s, set the current bank and address to the target, y to set? ", instr);
            fgets(tmpstr, sizeof(tmpstr), stdin);
            if( tmpstr[0] == 'y' )
            {
                lastAddr = addr;
                curBank = bank;
            }
        }
    }
    else
    {
        printf("The instruction at %s does not jump or reference memory.\n", findNameByAddr(tmpaddr));
    }
}

// Load in a rim tape.
// If no file is given, try the current file.
void
loadFn(char *filenameP)
{
int addr;
FileInfoP infoP;
char filename[1024];

    if( !filenameP && (infoP = getFileInfoP(0)) )     // try using the primary file name as a root
    {
        if( findRimFile(infoP, filename) )
        {
            openSourceFile(infoP);      // might as well open the source while we're here
            filenameP = filename;
        }
    }

    if( !filenameP )
    {
        printf("There are no current files, use an explicit name.\n");
    }

    if( (addr = loadTape(filenameP)) == LOADFAILED )
    {
        return;
    }
    else if( addr == LOADSTOP )
    {
        printf("Load of am1 tape with stop done, no starting address set.\n");
    }
    else
    {
        curStartAddr = addr;
        printf("Load done, starting address set to 0%06o.\n", addr);
    }
}

// Open a monitor dump file, step for the given count of instructions,
// writing the address and instruction for each step.
// The output is in pidp-1 new memory image format.
// Single step will be turned on if not already and its state restored when done.
// Breakpoints and watchpoints will not be recognized until completion.
void
monitorFn(int count, char *filenameP)
{
int addr, word;
bool singleState;
FILE *fP;

    if( pdp1P->run )
    {
        printf("Must be stopped to begin monitoring.\n");
        return;
    }

    if( !(fP = fopen(filenameP, "w")) )
    {
        printf("Can't open monitor output file '%s'.\n", filenameP);
        return;
    }

    singleState = AD1_SINGLE(pdp1P);
    AD1_SET_SINGLE(pdp1P);

    printf("Monitoring for %d instruction cycles.\n", count);
    while( count-- > 0 )
    {
        addr = pdp1P->epc | pdp1P->pc;
        word = pdp1P->core[addr];
        fprintf(fP, "%06o: %06o\n", addr, word);

        AD1_SET_CONTINUE(pdp1P);
        while( AD1_CONTINUE(pdp1P) )
        {
            usleep(1);       // wait for completion
        }
    }

    if( !singleState )
    {
        AD1_CLEAR_SINGLE(pdp1P);
    }

    fclose(fP);
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
