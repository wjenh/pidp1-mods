/*
 * This is a program to do real-time interactions with the pidp-1 when it is running in
 * shared memory mode.
 * It can read am1 symbol files and probe memory to examine the state of memory and other items.
 * It requires the pidp1-mods version of the pidp-1 with shared memory turned on.
 *
 * Original author: Bill Ezell (wje) pdp1@quackers.net
 *
 * 28-Feb-26 wje - initial version
 * 2-Mar-26 wje - fix memory mapping for banks other than 0
 * 4-Mar-26 wje - add new show formats
 * 7-Mar-26 wje - restrict some cmd args to decimal, add multi-line at one address support
 * 8-Mar-26 wje - show decoded instruction after watch or break hit if source not available,
 *      add address-of-symbol, add symbol table list, fix flex conversion bug, general cleanup
 *
*/
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

#define MEMFILE "/opt/pidp1-mods/coremem"
#define SHM_NAME "/pidp1"

PDP1P pdp1P;            // the emulator state in shared memory
char *am1NameP;
char *lstNameP;
char *symNameP;

char *fmt8P = "%06o";
char *fmt10P = "%06d";
char *fmt16P = "%05x";
char *fmt2P = "%018b";
char *symFileNameP;
char *sourceFileNameP;

// Names that start with the same character(s) can be selected by the order they are in below.
// The first match with the gven significance matches.
// For example, s will select step, se will select set.
// The token names come from y.tab.h, defined in parser.y.
Dispatch dispatchTable[] = {
    {"base", 3, BASE, baseHelp},
    {"bank", 2, BANK, bankHelp},
    {"break", 1, BREAK, breakHelp},
    {"continue", 1, CONTINUE, continueHelp},
    {"debug", 5, DEBUG, NIL},   // used to dump a bunch of info about the state when running shared
    {"delete", 2, DELETE, deleteHelp},
    {"disable", 2, DISABLE, disableHelp},
    {"exit", 2, EXIT, exitHelp},
    {"enable", 2, ENABLE, enableHelp},
    {"file", 2, SETFILE, fileHelp},
    {"help", 1, HELP, NIL},
    {"list", 1, LIST, listHelp},
    {"next", 1, NEXT, nextHelp},
    {"quit", 1, QUIT, exitHelp},
    {"set", 2, SET, setHelp},
    {"show", 2, SHOW, showHelp},
    {"start", 2, START, startHelp},
    {"stop", 3, STOP, stopHelp},
    {"step", 1, STEP, stepHelp},
    {"window", 2, WINDOW, windowHelp},
    {"watch", 1, WATCH, watchHelp},
    {0,0,0}
    };

// This is a hack to add extra help topics easily
Dispatch extraHelpTable[] = {
    {"numbers", 2, 0, numberHelp},
    {"expressions", 3, 0, expressionHelp},
    {"registers", 1, 0, registerHelp},
    {"addresses", 1, 0, addressHelp},
    {0,0,0}
    };

int memListCount;
void *memList[256];     // allocated memory

static bool flexShifted = true; // last shift state returned by flexToAscii(), true is UC,  reset on format change

DispatchP findCommand(DispatchP tableP, char *nameP);

BreakpointP isBreakpoint(int addr);
BreakpointP checkBreakpoints();
bool validateBreakpointNumber(int num);
void clearBreakpoint(BreakpointP brkP);
void deleteAllBreakpoints(void);
void listBreaks();

WatchP isWatch(int addr);
WatchP checkWatches();
bool validateWatchNumber(int num);
void clearWatch(WatchP watchP);
void deleteAllWatches(void);
void listWatches();

void disableAllWatchesAndBreakpoints(void);
void restoreAllWatchesAndBreakpoints(void);

int getCurrentPC(void);
bool loadFileData(void);
char *getFormat(int fmt);
char *getUnrestrictedFormat(int fmt);
void formatAndPrintOne(int fmt, int value);
void formatAndPrintTwo(int fmt1, int addr, int fmt2,  int value);
void printAscii(char ch);
bool printFlex(bool shifted, char ch);
bool loadMemoryFromFile(char *filenameP, Word memory[], Word memSize);
void usage(void);

static void leave(int, void *);

extern int brkCount;    // number of set breakpoints
extern int watchCount;  // number of set watches
extern int base;        // current number base
extern int lastFormat;  // the last format type used
extern int curBank;     // set by the bank cmd

extern int yydebug;
extern int yy_flex_debug;
extern int lastAddr;
extern char *am1NameP;
extern char *lstNameP;
extern char *symNameP;

extern SymbolP findSymbolByName(char *nameP);
extern int parseAndExecute(char *lineP);
extern MapEntryP getLinesFromAddress(int addr);
extern int signExtend(int oc);
extern int twosCompl(int val);
extern int onesCompl(int val);
extern int32_t findAddrByName(char *nameP);
extern char *findNameByAddr(u32 addr);
extern char *decodeInstr(int value, int addr, char *addrStrP, char *reslltP);
extern bool loadSymbols(char *filenameP);
extern void clearFiles(void);
extern void clearSymbols(void);
extern void closeListFile(void);
extern bool resolveFiles(char *nameP, char **am1PP, char **symPP, char **lstPP);
extern bool isFileMapped(void);
extern bool isMemMapped(void);
extern bool loadFileMap(bool fromLst, char *filenameP);
extern void clearSymbols(void);
extern void closeFile(void);
extern int getLineCount(void);
extern bool printLines(MapEntryP entryP);
extern bool printNextLine(void);
extern int getCurrentLineNumber(void);
extern int getNumber(char *strP, int base);
extern void listFn(int arg, MapEntryP mapP);
extern int flexToAscii(int ch, bool *shiftP);

int
main(int argc, char **argv)
{
int i;
int shmFd;
int inFd;
int exitStatus;
bool wantDelay;
bool testMode;
char *cP;
DispatchP cmdP;
BreakpointP activeBrkP; // we hit a breakpoint, this is it
WatchP activeWatchP;    // we hit a watch, this is it
MapEntryP mapP;
fd_set read_fds;
struct timeval timeout;
char line[256];

    yy_flex_debug = yydebug = 0;
    symFileNameP = NIL;
    testMode = false;

    /* do the command line processing */
    ++argv;
    --argc;

    while(argc && (**argv == '-'))                        /* look for directives */
    {
        for(cP = *argv + 1; *cP;)
        {
            switch(*cP++)
            {
            case 'T':
                testMode = 1;
                break;

            case 'v':
                printf("ad1 version %s\n", VERSION);
                exit(0);

            case 'y':
                yydebug = 1;
                break;

            case 'x':
                yy_flex_debug = 1;
                break;

            default:
                usage();
                break;
            }
        }

        --argc;
        ++argv;
    }

    if( argc > 1)
    {
        usage();
    }

    if( argc == 1 )
    {
        resolveFiles(*argv, &am1NameP, &symNameP, &lstNameP);
        loadFileData();         // do before loading symbols!
        loadSymbols(symNameP);
    }

    // Initialize the file descriptor set
    inFd = STDIN_FILENO;            // File descriptor for standard input
    FD_ZERO(&read_fds);

    // Set the timeout value
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;

    if( testMode )
    {
        // Just a local copy for standalone testing.
        PDP1 fakePDP1;
        pdp1P = &fakePDP1;
        memset(pdp1P, 0, sizeof(PDP1));
        if( !loadMemoryFromFile(MEMFILE, pdp1P->core, MAXMEM) )
        {
            printf("Can't load memory image from file '%s', memory may be empty.", MEMFILE);
        }
    }
    else
    {
        shmFd = shm_open(SHM_NAME, O_RDWR, 0666);
        if( shmFd < 0 )
        {
            fprintf(stderr, "shm_open of %s failed, is pdp1 running?\n", SHM_NAME);
            exit(1);
        }
        else
        {
            pdp1P = mmap(NIL, sizeof(PDP1), PROT_READ | PROT_WRITE, MAP_SHARED, shmFd, 0);
            if( pdp1P == NIL )
            {
                fprintf(stderr, "mmap failed\n");
                close(shmFd);
                exit(1);
            }

            close(shmFd);
        }
    }

    // We might have breakpoints already set because of a prior exit.
    restoreAllWatchesAndBreakpoints();

    on_exit(leave, 0);
    activeBrkP = NIL;
    activeWatchP = NIL;
    base = OCTAL;               // default is octal
    lastFormat = OCTAL;
    printf("Type 'help' for help.\n");

    while( true )
    {
        write(STDOUT_FILENO, "Cmd? ", 5);   // this silliness to get around buffering issues with select

        while( true )
        {
            // We only need to have timeouts if we have a reason
            if( (brkCount > 0) || (watchCount > 0) )
            {
                FD_SET(inFd, &read_fds);    // has to be reset each time
                i = select(inFd + 1, &read_fds, NULL, NULL, &timeout);
            }
            else
            {
                break;
            }

            if( i == 0 )                // timeout, do our breakpoint cheks
            {
                if( (activeBrkP = checkBreakpoints()) )
                {
                    break;              // hit one
                }

                if( (activeWatchP = checkWatches()) )
                {
                    break;              // hit one
                }
            }
            else if( i < 0 )
            {
                printf("Error in select(), terminating,\n");
                exit(1);
            }
            else if( FD_ISSET(inFd, &read_fds) )
            {
                break;      // input ready
            }
        }

        if( activeBrkP )
        {
            // breakpoint hit
            printf("\nBreakpoint %d hit", activeBrkP->number);
            lastAddr = activeBrkP->address;
            if( (mapP = getLinesFromAddress(activeBrkP->address)) > 0 )
            {
                printf(" at line %d:\n", mapP->lineNo);
                printLines(mapP);
                NEWLINE;
            }
            else
            {
                i = activeBrkP->address;
                cP = findNameByAddr(i);
                i = pdp1P->core[i];
                decodeInstr(i, i & 0777, cP, line);
                printf(": %s\n", line);
            }

            write(STDOUT_FILENO, "Cmd? ", 5);
            activeBrkP = NIL;
        }
        else if( activeWatchP )
        {
            // watch hit
            printf("\nWatch %d hit", activeWatchP->number);
            lastAddr = activeWatchP->address;
            if( (mapP = getLinesFromAddress(activeWatchP->address)) > 0 )
            {
                printf(" at line %d:\n", mapP->lineNo);
                printLines(mapP);
                NEWLINE;
            }
            else
            {
                i = activeBrkP->address;
                cP = findNameByAddr(i);
                i = pdp1P->core[i];
                decodeInstr(i, i & 0777, cP, line);
                printf(": %s\n", line);
            }

            write(STDOUT_FILENO, "Cmd? ", 5);
            activeWatchP = NIL;
        }

        if( !fgets(line, sizeof(line), stdin) )
        {
            exitStatus = QUIT;
            break;
        }

        fflush(stdout);

        if( line[0] == '\n' )
        {
            // empty line, means same as list with no arg.
            listFn(NOARG, NIL);
            continue;
        }

        cP = strchr(line, '\n');
        *cP = NUL;

        exitStatus = parseAndExecute(line);
        if( (exitStatus == EXIT) || (exitStatus == QUIT) )
        {
            break;          // exit done
        }
    }

    exit(exitStatus);
}

// Given a numeric format as for strtol(), return a format string for printf().
// If it isn't one of binary, octal, decimal, or hex, return octal.
char *
getFormat(int fmt)
{
    if( !fmt )
    {
        return(fmt8P);
    }

    switch( fmt )
    {
    case BINARY:
        return(fmt2P);
    case OCTAL:
        return(fmt8P);
    case DECIMAL:
        return(fmt10P);
    case HEX:
        return(fmt16P);
    default:
        return(fmt8P);
    }
}

// Given a numeric format as for strtol(), return a format string for printf()
// that doesn't have any number of digits, e.g. %d.
char *
getUnrestrictedFormat(int fmt)
{
    switch( fmt )
    {
    case BINARY:
        return("%b");
    case 0:
    case OCTAL:
        return("%o");
    case DECIMAL:
        return("%d");
    case HEX:
        return("%x");
    default:
        return("%o");
    }
}

// Print a value, no newline.
void
formatAndPrintOne(int fmt, int value)
{
int addr;
char c1, c2, c3;
char *cP;
char tmpstr[128];

    if( (fmt != FLEX) && (fmt != AUTOBASE) )
    {
        flexShifted = true;    // back to upper case
    }

    if( fmt == SYMBOLIC )
    {
        if( (cP = findNameByAddr(value)) )
        {
            printf("%s", cP);
        }
        else
        {
            printf(getFormat(lastFormat), value);
        }

        return;
    }
    else if( fmt == ADDRESS )
    {
        if( (cP = findNameByAddr(value)) )
        {
            printf("%s", cP);
        }
        else
        {
            // Leading addresses are always octal
            printf(getFormat(OCTAL), value);
        }

        return;
    }
    else if( fmt == INSTRUCTION )
    {
        // Might be an address?
        addr = (value & 0777) | (curBank << 12);
        cP = findNameByAddr(addr);
        decodeInstr(value, value & 0777, cP, tmpstr);
        printf("%s",  tmpstr);
    }
    else if( fmt == ASCII )
    {
        // Ascii is packed two chars per word, 9 bits each with the high bit ignored.
        c1 = (value & 0377000) >> 9;
        c2 = value & 0377;
        PRINTCH('\'');
        printAscii(c1);
        printAscii(c2);
        PRINTCH('\'');
    }
    else if( fmt == FLEX )
    {
        c1 = (value & 0770000) >> 12;
        c2 = (value & 07700) >> 6;
        c3 = value & 077;
        PRINTCH('\'');
        flexShifted = printFlex(flexShifted, c1);
        flexShifted = printFlex(flexShifted, c2);
        flexShifted = printFlex(flexShifted, c3);
        PRINTCH('\'');
    }
    else if( fmt == ONESCMPL )
    {
        printf("%d", twosCompl(signExtend(value)));     // always decimal
    }
    else
    {
        printf(getFormat((fmt == AUTOBASE)?lastFormat:fmt), value);
    }

    switch( fmt )
    {
    case AUTOBASE:
    case SYMBOLIC:              // should never see this here, but be sure
    case ADDRESS:               // same
        return;                 // we don't want to change the last format for these

    default:
        lastFormat = fmt;
        break;
    }
}

// Print one ascii char, possibly null
void
printAscii(char ch)
{
    if( ch )
    {
        printf("%c", ch);
    }
    else
    {
        printf("\\0");
    }
}

// Print a flex char as ascii using the passed shift state, return the possibly-changed shift state.
bool
printFlex(bool shifted, char ch)
{
int chr;
bool newShift;

    newShift = shifted;
    chr = flexToAscii(ch, &newShift);
    switch( chr )
    {
    case NOCHAR:
        PRINTCH(' ');
        break;
    case UCS:
        printf("(UC)");
        newShift = true;
        break;
    case LCS:
        printf("(lc)");
        newShift = false;
        break;
    default:
        PRINTCH(chr);
        break;
    }

    return( newShift );
}

// Print an address and a value, also prints a newline.
void
formatAndPrintTwo(int fmt1, int addr, int fmt2, int value)
{
bool flexShift;
char *cP;

    // We don't want to mess up the flex shift state for the first output
    flexShift = flexShifted;
    formatAndPrintOne(fmt1, addr);   // print as symbol if it has one, else just the addr
    printf(": ");
    flexShifted = flexShift;
    formatAndPrintOne(fmt2, value);
}

// Clear all state for a breakpoint.
void
clearBreakpoint(BreakpointP brkP)
{
    if( !brkP->isSet )
    {
        return;
    }

    brkP->isSet = brkP->isEnabled = false;

    brkP->address = 0;  // not necessary, but for completeness
    if( --brkCount <= 0 )
    {
        AD1_DISABLE_BREAKPOINTS(pdp1P);
        brkCount = 0;
    }
}

// Check for a valid breakpoint nunber.
// If ok, return true, else false.
bool
validateBreakpointNumber(int num)
{
    if( (num < 1) || (num > AD1_NUM_BREAKPOINTS) )
    {
        printf("A breakpont number must be between 1 and %d\n", AD1_NUM_BREAKPOINTS);
        return( false );
    }
    
    return(true);
}

// See if the address has a breakpoint set on it.
// If so, return the breakpoint else return nil.
BreakpointP
isBreakpoint(int addr)
{
int i;
BreakpointP brkP;

    brkP = pdp1P->ad1Breakpoints;
    for( i = 0; i < AD1_NUM_BREAKPOINTS; ++i, ++brkP )
    {
        // Check the pc and mem addresses
        if( brkP->isSet && (brkP->address == addr) )
        {
            return(brkP);
        }
    }

    return(NIL);
}

BreakpointP
checkBreakpoints()
{
int i;
BreakpointP brkP;

    // see if a breakpoint was signaled
    if( !pdp1P->run && AD1_BREAKPOINT_HIT(pdp1P) )
    {
        brkP = &(pdp1P->ad1Breakpoints[pdp1P->ad1brkNo]);
        AD1_CLEAR_BREAKPOINT_HIT(pdp1P);
        return( brkP );
    }

    return(NIL);
}

// called on termination by the exit command
void
disableAllWatchesAndBreakpoints()
{
int i;
BreakpointP brkP;
WatchP watchP;

    brkP = pdp1P->ad1Breakpoints;

    for( i = 0; i < AD1_NUM_BREAKPOINTS; ++i, ++brkP )
    {
        brkP->isEnabled = false;
    }

    watchP = pdp1P->ad1Watches;

    for( i = 0; i < AD1_NUM_WATCHES; ++i, ++watchP )
    {
        watchP->isEnabled = false;;
    }
}

// called on startup
void
restoreAllWatchesAndBreakpoints()
{
int i;
BreakpointP brkP;
WatchP watchP;

    watchCount = brkCount = 0;

    brkP = pdp1P->ad1Breakpoints;

    for( i = 0; i < AD1_NUM_BREAKPOINTS; ++i, ++brkP )
    {
        if( brkP->isSet )
        {
            ++brkCount;
        }
    }

    watchP = pdp1P->ad1Watches;

    for( i = 0; i < AD1_NUM_WATCHES; ++i, ++watchP )
    {
        if( watchP->isSet )
        {
            ++watchCount;
        }
    }
}
// Does what is says.
void
deleteAllBreakpoints()
{
int i;
BreakpointP brkP;

    brkP = pdp1P->ad1Breakpoints;

    for( i = 0; i < AD1_NUM_BREAKPOINTS; ++i, ++brkP )
    {
        clearBreakpoint(brkP);
    }

    AD1_DISABLE_BREAKPOINTS(pdp1P);
    brkCount = 0;
}

void
listBreaks()
{
int i;
BreakpointP brkP;

    if( brkCount > 0 )
    {
        for( i = 0, brkP = pdp1P->ad1Breakpoints; i < AD1_NUM_BREAKPOINTS; ++i, ++brkP )
        {
            if( brkP->isSet )
            {
                printf("%d: ", brkP->number);
                formatAndPrintOne(SYMBOLIC, brkP->address);
                printf(" ,count %d ,currently %d,", brkP->count, brkP->curCount);
                printf(" %s\n", (brkP->isEnabled)?"enabled":"disabled");
            }
        }
    }
    else
    {
        printf("No breakpoints set.\n");
    }
}

// Validate a string as a number.
// If valid, convert the range 1 to AD1_NUM_BREAKPOINTS to 0 to AD1_NUM_BREAKPOINTS -1 and return it.
// If invalid, return -1;
int
getBreakpointNumber(char *cP)
{
int bpno;

    if( !isdigit(*cP) )
    {
        printf("Breakpoint numbers must be decimal number.\n");
        return(-1);
    }

    bpno = atoi(cP);
    if( (bpno < 1) || (bpno > AD1_NUM_BREAKPOINTS) )
    {
        printf("Breakpoint numbers must be decimal 1-%d.\n", AD1_NUM_BREAKPOINTS);
        return(-1);
    }

    return( bpno-1 );
}

// Clear all state for a watch
void
clearWatch(WatchP watchP)
{
    if( !watchP->isSet )
    {
        return;
    }

    watchP->isSet = watchP->isEnabled = false;

    watchP->address = 0;  // not necessary, but for completeness
    watchP->value = 0;

    if( --watchCount <= 0 )
    {
        AD1_DISABLE_WATCHES(pdp1P);
        watchCount = 0;
    }
}

// Check for a valid watch nunber.
// If ok, return true, else false.
bool
validateWatchNumber(int num)
{
    if( (num < 1) || (num > AD1_NUM_WATCHES) )
    {
        printf("A watch number must be between 1 and %d\n", AD1_NUM_WATCHES);
        return( false );
    }
    
    return(true);
}

// See if the address has a watch set on it.
// If so, return the watch else return nil.
WatchP
isWatch(int addr)
{
int i;
WatchP watchP;

    watchP = pdp1P->ad1Watches;
    for( i = 0; i < AD1_NUM_WATCHES; ++i, ++watchP )
    {
        // Check the pc and mem addresses
        if( watchP->isSet && (watchP->address == addr) )
        {
            return(watchP);
        }
    }

    return(NIL);
}

WatchP
checkWatches()
{
int i;
WatchP watchP;

    // see if a watch was signaled
    if( !pdp1P->run && AD1_WATCH_HIT(pdp1P) )
    {
        watchP = &(pdp1P->ad1Watches[pdp1P->ad1watchNo]);
        AD1_CLEAR_WATCH_HIT(pdp1P);
        return( watchP );
    }

    return(NIL);
}

// Does what is says.
void
deleteAllWatches()
{
int i;
WatchP watchP;

    watchP = pdp1P->ad1Watches;

    for( i = 0; i < AD1_NUM_WATCHES; ++i, ++watchP )
    {
        clearWatch(watchP);
    }

    // Should already be done, but be sure
    AD1_DISABLE_WATCHES(pdp1P);
    watchCount = 0;
}

void
listWatches()
{
int i;
WatchP watchP;

    if( watchCount )
    {
        for( i = 0, watchP = pdp1P->ad1Watches; i < AD1_NUM_WATCHES; ++i, ++watchP )
        {
            if( watchP->isSet )
            {
                printf("%d: address ", watchP->number);
                formatAndPrintOne(SYMBOLIC, watchP->address);
                if( watchP->onAny )
                {
                    printf(" any value");
                }
                else
                {
                    printf(" value %06o", watchP->value);
                }
                printf(" %s\n", (watchP->isEnabled)?"enabled":"disabled");
            }
        }
    }
    else
    {
        printf("No watches set.\n");
    }
}

// Validate a string as a number.
// If valid, convert the range 1 to AD1_NUM_WATCHES to 0 to AD1_NUM_WATCHES -1 and return it.
// If invalid, return -1;
int
getWatchNumber(char *cP)
{
int watchno;

    if( !isdigit(*cP) )
    {
        printf("Watch numbers must be a number.\n");
        return(-1);
    }

    watchno = atoi(cP);
    if( (watchno < 1) || (watchno > AD1_NUM_WATCHES) )
    {
        printf("Watch numbers must be decimal 1-%d.\n", AD1_NUM_WATCHES);
        return(-1);
    }

    return( watchno-1 );
}

// Search for a command, applying significant char matching.
// If found, return the dispatch entry, else NIL.
DispatchP
findCommand(DispatchP tableP, char *nameP)
{
int i;
DispatchP cmdP;

    for( cmdP = tableP; cmdP->nameP != NIL; ++cmdP )
    {
        i = strlen(nameP);

        if( !strncmp(nameP, cmdP->nameP, i) && (i >= cmdP->significant) )
        {
            return( cmdP );
        }
    }

    return( NIL );
}

// Malloc memory, keep track of it for freeing via freeMem()
void *
allocMem(int bytes)
{
void *memP;

    memP = malloc(bytes);
    memList[memListCount++] = memP;
    return( memP );
}

void
freeMem()
{
int i;

    for( i = 0; i < memListCount; ++i )
    {
        if( memList[i] )
        {
            free( memList[i] );
            memList[i] = NIL;
        }
    }

    memListCount = 0;
}

bool
loadFileData()
{
    if( !isFileMapped() )       // try to initialize it
    {
        // Try for a lst file
        if( loadFileMap(true, lstNameP))
        {
            printf("Line numbers and addresses loaded from '%s'.\n", lstNameP);
        }
        else if( loadFileMap(false, am1NameP) )
        {
            // Try for a .am1 instead
            printf("Line numbers from '%s', symbol mapping not available..\n", am1NameP);
        }

        if( !isFileMapped() )
        {
            printf("Can't find a listing or source file, set a file with the file command.\n");
            return(false);
        }
    }

    return(true);
}

// Load a pidp-1 memory save file into the test memory.
// memSize is in memory words.
bool
loadMemoryFromFile(char *filenameP, Word memory[], Word memSize)
{
FILE *fP;
char *sP;
char buf[100];
Word addr;
Word data;

    if( (fP = fopen(filenameP, "r")) == NIL )
    {
        return(false);     // can't load it
    }

    addr = 0;

    while( (sP = fgets(buf, 100, fP)) )
    {
        while( *sP )
        {
            if(*sP  == ';')
            {
                break;
            }
            else if( ('0' <= *sP)  && (*sP  <= '7') )
            {
                data = strtol(sP, &sP, 8);  // data word

                if( *sP  == ':' )
                {
                    addr = data;            // new address
                    sP++;
                }
                else if( addr < memSize )
                {
                    memory[addr++] = data;
                }
                else
                {
                    printf("Bad data in memory file , loading stopped.\n");
                    fclose(fP);
                    return( false );
                }
            }
            else
            {
                sP++;
            }
        }
    }

    fclose(fP);
    return( true );
}

// Return the current full address of the pc in the emulator, pd and epc.
int
getCurrentPC()
{
    return( (pdp1P->epc & 0170000) | (pdp1P->pc & 07777) );
}

void
leave(int status, void *ignore)
{
    // Exit will preserve all the breakpoints, but disable them
    if( status == EXIT )
    {
        disableAllWatchesAndBreakpoints();
    }
    else
    {
        deleteAllBreakpoints();
        deleteAllWatches();
    }
}

void
usage()
{
    printf("Usage: ad1 [-v] [-y] [-x] [-T] [filename]\n");
    printf("-v prints the version and exits\n");
    printf("-y enables yacc debugging\n");
    printf("-x enables lex debugging\n");
    printf("-T enables test mode\n");
    exit(1);
}
