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

#include "ad1.h"
#include "helpmsgs.h"
#include "y.tab.h"
#include "../Disassembler/decode_instruction.h"

int lastAddr;
int base;               // default input number base, changed by the base command
int lastFormat;         // last format used, 0 means use base
int curStartAddr;       // set by the start or load commands
int curBank;            // set by the bank cmd
int windowSize = 6;     // default window size

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

extern char *symNameP;
extern Dispatch dispatchTable[];
extern Dispatch extraHelpTable[];
extern int isBreakpoint(int addr);

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
extern void deleteAllBreakpoints(void);

extern void listWatches(void);
extern bool validateWatchNumber(int num);
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

// Report a status from the emulator that the caller has no wording of its own for.
// Returns true if the status was ok.
static bool
statusOk(int status)
{
    if( status == AD1P_ST_OK )
    {
        return( true );
    }

    printf("The emulator refused that: %s.\n", ad1StatusText(status));
    return( false );
}

// True if the machine is in extend mode, where an indirect names a bank.
static bool
extendedMode(void)
{
uint32_t state[AD1P_STATE_WORDS];

    tgtGetState(state);
    return( state[AD1P_STATE_EXD] != 0 );
}

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
        val = (int)tgtRead(addr);
        formatAndPrintTwo(ADDRESS, addr, base, val);
        NEWLINE;
    }
}

void
showRegisterFn(int reg, int base)
{
u32 val;
char *nameP;
uint32_t state[AD1P_STATE_WORDS];

    tgtGetState(state);

    switch( reg )
    {
    case ACREG:
        nameP = "AC";
        val = state[AD1P_STATE_AC];
        break;
    case IOREG:
        nameP = "IO";
        val = state[AD1P_STATE_IO];
        break;
    case PCREG:
        nameP = "PC";
        val = state[AD1P_STATE_PC];
        break;
    case TWREG:
        nameP = "Test word switches";
        val = state[AD1P_STATE_TW];
        break;
    case PFREG:
        printf("Program flags: %06b\n", state[AD1P_STATE_PF]);
        return;
    case SSREG:
        printf("Sense switches: %06b\n", state[AD1P_STATE_SS]);
        return;
    case ASREG:
        nameP = "Address switches";
        val = state[AD1P_STATE_TA];
        break;
    case MAREG:
        nameP = "MA";
        val = state[AD1P_STATE_MA];
        break;
    case MBREG:
        nameP = "MB";
        val = state[AD1P_STATE_MB];
        break;
    case DOT:
        nameP = ".";
        val = tgtRead(lastAddr);
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
int brkNo;
bool clr;

    if( (type != REGISTER) && (brkNo = isBreakpoint(addr)) )
    {
        printf("The address cannot be set unless breakpoint %d is deleted first.\n", brkNo);
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

            if( statusOk(tgtSetReg(AD1P_REG_PC, AD1P_OP_ASSIGN, value)) )
            {
                lastAddr = value;
            }
            break;

        case ACREG:
            statusOk(tgtSetReg(AD1P_REG_AC, AD1P_OP_ASSIGN, onesCompl(value)));
            break;

        case IOREG:
            statusOk(tgtSetReg(AD1P_REG_IO, AD1P_OP_ASSIGN, onesCompl(value)));
            break;

        case PFREG:
            if( value == 0 )
            {
                statusOk(tgtSetReg(AD1P_REG_PF, AD1P_OP_ASSIGN, 0));
            }
            else if( value > 0x3F )
            {
                printf("The value must be between 1 and 077, hex 3F, decimal 63.\n");
                printf("The bit pattern determines which flags are set.\n");
            }
            else
            {
                statusOk(tgtSetReg(AD1P_REG_PF, AD1P_OP_ASSIGN, value));
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
                break;
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
            statusOk(tgtSetReg(AD1P_REG_PF, clr?AD1P_OP_CLEAR:AD1P_OP_OR, 1 << flag));
            break;

        default:
            printf("That register cannot be set.\n");
            break;
        }
    }
    else
    {
        if( statusOk(tgtWrite(addr, value)) )
        {
            lastAddr = addr;
        }
    }
}

void
startFn(int addr)
{
    if( addr < 0 )
    {
        printf("No start address has been set, give one or load a tape.\n");
    }
    else if( statusOk(tgtStart(addr)) )
    {
        curStartAddr = lastAddr = addr;
    }
}

void
stopFn(void)
{
uint32_t pc;

    // The reply comes after the machine has halted, so the pc is where it stopped.
    if( statusOk(tgtStop(&pc)) )
    {
        lastAddr = pc;
    }
}

// If count is the number of instruction cycles to step.
// A breakpoint or watchpoint hit will end stepping immediately.
void
stepFn(int count)
{
int stalled;
uint32_t state[AD1P_STATE_WORDS];
int status;
Ad1StepResult res;
MapEntryP entryP;

    if( tgtIsLocal() )
    {
        printf("Stepping is not available in test mode.\n");
        return;
    }

    tgtGetState(state);
    if( state[AD1P_STATE_RUN] )
    {
        printf("Must be stopped to step.\n");
    }
    else
    {
        if( count == BADNUM )
        {
            count = 1;
        }

        // The emulator gives up on a very long run of steps and says how far it got, so ask again
        // for the rest. It leaves the sticky single-step state set, as the panel would.
        stalled = 0;
        while( count > 0 )
        {
            status = tgtStep(count, false, &res);
            if( (status != AD1P_ST_OK) && (status != AD1P_ST_TIMEOUT) )
            {
                statusOk(status);
                return;
            }

            if( res.reason == AD1P_END_HIT )
            {
                return;             // stop now, the main loop reports the hit
            }

            count -= (res.done > (uint32_t)count) ? count : (int)res.done;
            if( (res.done == 0) && (++stalled >= 3) )
            {
                printf("The pdp-1 is not stepping.\n");
                return;
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
    statusOk(tgtContinue());
}

void
setBankFn(int bankno)
{
    if( (bankno < 0) || (bankno >= MEMBANKS) )
    {
        printf("A bank number must be 0-%d decimal or the hex or octal equivalent.\n", MEMBANKS - 1);
        return;
    }

    curBank = bankno;
}

void
setBpFn(int addr, int count)
{
int status;
uint32_t number;

    if( count == BADNUM )
    {
        count = 1;          // no count was given, default to 1
    }

    status = tgtBpSet(addr, count, &number);
    if( status == AD1P_ST_NO_SLOT )
    {
        printf("No breakpoints are left, delete one first\n");
    }
    else
    {
        statusOk(status);
    }
}

// 0 means all of them
void
deleteBpFn(int bpno)
{
int status;
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
        status = tgtBpDelete(bpno);
        if( status == AD1P_ST_NOT_SET )
        {
            printf("Breakpoint %d is not set.\n", bpno);
        }
        else
        {
            statusOk(status);
        }
    }
}

void
enableBpFn(int bpno)
{
int status;

    if( !validateBreakpointNumber(bpno) )
    {
        return;
    }

    status = tgtBpEnable(bpno);
    if( status == AD1P_ST_ALREADY )
    {
        printf("Breakpoint %d is already enabled.\n", bpno);
    }
    else if( status == AD1P_ST_NOT_SET )
    {
        printf("Breakpoint %d is not set, can't enable it.\n", bpno);
    }
    else
    {
        statusOk(status);
    }
}

void
disableBpFn(int bpno)
{
int status;

    if( !validateBreakpointNumber(bpno) )
    {
        return;
    }

    status = tgtBpDisable(bpno);
    if( status == AD1P_ST_ALREADY )
    {
        printf("Breakpoint %d is already diabled.\n", bpno);
    }
    else if( status == AD1P_ST_NOT_SET )
    {
        printf("Breakpoint %d is not set, can't disable it.\n", bpno);
    }
    else
    {
        statusOk(status);
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
    word = tgtRead(addr);

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
            addr = tgtRead(addr);
            formatAndPrintOne(SYMBOLIC, FULLADDR(bank, addr));
            NEWLINE;

            // The behavior of indirect depends upon whether or not eem is in effect.
            // If it is, there is no subsequent indirection.
            if( extendedMode() )
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
                tmpaddr = tgtRead(FULLADDR(bank, addr));
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
        return;
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
int chunk;
int stalled;
int status;
uint32_t i;
uint32_t state[AD1P_STATE_WORDS];
bool singleState;
FILE *fP;
Ad1StepResult res;

    if( tgtIsLocal() )
    {
        printf("Monitoring is not available in test mode.\n");
        return;
    }

    tgtGetState(state);
    if( state[AD1P_STATE_RUN] )
    {
        printf("Must be stopped to begin monitoring.\n");
        return;
    }

    if( !(fP = fopen(filenameP, "w")) )
    {
        printf("Can't open monitor output file '%s'.\n", filenameP);
        return;
    }

    singleState = (state[AD1P_STATE_SINGLE] != 0);

    printf("Monitoring for %d instruction cycles.\n", count);

    // The emulator records the address and word before each step, up to a limit per request. A
    // breakpoint or watch hit ends a request early with the hit still latched, so the next
    // request steps one more and ends the same way; that keeps the run going as the old
    // monitor did, and the hit is reported once it is over.
    stalled = 0;
    while( count > 0 )
    {
        chunk = (count > AD1P_MAX_RECORDS) ? AD1P_MAX_RECORDS : count;
        status = tgtStep(chunk, true, &res);
        if( (status != AD1P_ST_OK) && (status != AD1P_ST_TIMEOUT) )
        {
            statusOk(status);
            break;
        }

        for( i = 0; i < res.nRecords; ++i )
        {
            fprintf(fP, "%06o: %06o\n", res.recordsP[i * 2], res.recordsP[(i * 2) + 1]);
        }

        count -= (res.done > (uint32_t)chunk) ? chunk : (int)res.done;
        if( (res.done == 0) && (++stalled >= 3) )
        {
            printf("The pdp-1 is not stepping.\n");
            break;
        }
    }

    if( !singleState )
    {
        tgtClearSingle();
    }

    fclose(fP);
}

void
setWatchFn(int addr, int value)
{
int status;
uint32_t number;

    if( value == BADNUM )
    {
        status = tgtWatchSet(addr & 0177777, true, 0, &number);
    }
    else
    {
        status = tgtWatchSet(addr & 0177777, false, value, &number);
    }

    if( status == AD1P_ST_NO_SLOT )
    {
        printf("No watches are left, delete one first\n");
    }
    else
    {
        statusOk(status);
    }
}

void
deleteWatchFn(int num)
{
int status;
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

        status = tgtWatchDelete(num);
        if( status == AD1P_ST_NOT_SET )
        {
            printf("Watch %d is not set.\n", num);
        }
        else
        {
            statusOk(status);
        }
    }
}

void
enableWatchFn(int num)
{
int status;

    if( !validateWatchNumber(num) )
    {
        return;
    }

    // The emulator refreshes the remembered value so the watch does not fire until the next change.
    status = tgtWatchEnable(num);
    if( status == AD1P_ST_ALREADY )
    {
        printf("Watch %d is already enabled.\n", num);
    }
    else if( status == AD1P_ST_NOT_SET )
    {
        printf("Watch %d is not set, can't enable it.\n", num);
    }
    else
    {
        statusOk(status);
    }
}

void
disableWatchFn(int num)
{
int status;

    if( !validateWatchNumber(num) )
    {
        return;
    }

    status = tgtWatchDisable(num);
    if( status == AD1P_ST_ALREADY )
    {
        printf("Watch %d is already diabled.\n", num);
    }
    else if( status == AD1P_ST_NOT_SET )
    {
        printf("Watch %d is not set, can't disable it.\n", num);
    }
    else
    {
        statusOk(status);
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
uint32_t val;
uint32_t state[AD1P_STATE_WORDS];
Ad1BpEntry bps[AD1_NUM_BREAKPOINTS];
Ad1WatchEntry watches[AD1_NUM_WATCHES];

    tgtGetState(state);
    printf("pc %06o exd %d run %d single %d dropped events %d\n", state[AD1P_STATE_PC],
        state[AD1P_STATE_EXD], state[AD1P_STATE_RUN], state[AD1P_STATE_SINGLE],
        state[AD1P_STATE_DROPPED]);
    printf("bps %s brkno %d brkhit %d\n", state[AD1P_STATE_BRK_ENABLED]?"on":"off",
        state[AD1P_STATE_BRK_NO], state[AD1P_STATE_BRK_HIT]);
    printf("watches %s watchno %d watchhit %d\n", state[AD1P_STATE_WATCH_ENABLED]?"on":"off",
        state[AD1P_STATE_WATCH_NO], state[AD1P_STATE_WATCH_HIT]);

    if( tgtBpList(bps) == AD1P_ST_OK )
    {
        for(i = 0; i < AD1_NUM_BREAKPOINTS; ++i )
        {
            printf("bkp %d set %d enabled %d address %06o\n", i+1, bps[i].isSet, bps[i].isEnabled, bps[i].address);
        }
    }

    if( tgtWatchList(watches) == AD1P_ST_OK )
    {
        for(i = 0; i < AD1_NUM_WATCHES; ++i )
        {
            printf("watch %d set %d enabled %d address %06o,",
                i+1, watches[i].isSet, watches[i].isEnabled, watches[i].address);

            val = tgtRead(watches[i].address);

            if( watches[i].onAnyChange )
            {
                printf(" value any, value %06o, lastval %06o, memval %06o\n",
                    watches[i].value, watches[i].lastValue, val);
            }
            else
            {
                printf(" value %06o, lastVal %06o, memval %06o\n", watches[i].value, watches[i].lastValue, val);
            }
        }
    }
}
