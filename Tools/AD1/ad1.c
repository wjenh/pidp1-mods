/*
 * This is a program to do real-time interactions with a running pidp-1 over its TCP debugger
 * link.
 * It can read am1 symbol files and probe memory to examine the state of memory and other items.
 * It requires the pidp1-mods version of the pidp-1, whose debugger port is on by default.
 *
 * Original author: Bill Ezell (wje) pdp1@quackers.net
 *
 * 28-Feb-26 wje - initial version
 * 2-Mar-26 wje - fix memory mapping for banks other than 0
 * 4-Mar-26 wje - add new show formats
 * 7-Mar-26 wje - restrict some cmd args to decimal, add multi-line at one address support
 * 8-Mar-26 wje - show decoded instruction after watch or break hit if source not available,
 *   add address-of-symbol, add symbol table list, fix flex conversion bug, general cleanup
 * 11-Mar-26 wje rework single-step logic, make sure to clear single_inst on exit
 * 12-Mar-26 wje minor change to show line numbers better, updated in-app help.
 * 13-Mar-26 wje minor change to catch sigint and sigquit and clean up
 * 16-Mar-26 wje major chage to add multiple source file support
 * 17-Mar-26 wje align -0 processing with am1 and the PDP-1, add mod operator, add tape loading
 * 18-Mar-26 wje unify lexing of file name for file and load, fix typos in help text
 * 22-Mar-26 wje update to use new decodeInstr()
 * 4-Apr-26 wje add format command to see current format or set it
 * 5-Apr-26 wje add trace command to follow references, including indirects
 * 6-Apr-26 wje add monitor command to dump an execution sequence to a file
 * 10-Apr-26 wje use word mask macros for bankd and addr parts of instruction, add optional address for trace
 * 27-Apr-26 wje fix lexing issue with decimal-only commands
 * 23-Jun-26 wje set the current line number to the breakpoint or watch line when it is hit,
 *   load rim from the current file if no file given, try for a rim or a bin.
 *   Rework location printing when a breakpoint or watchpoint is hit.
 * 20-Aug-26 wje swap the , and : for file and bank separators to be consistent with am1,
 *   increase max lines per file to 10K for line mapping array.
 * 2-Sep-26 wje clean up some of the exit handlers
 * 20-Sep-26 Claude - talk to the emulator over its TCP debugger link instead of shared memory
*/
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <signal.h>
#include <errno.h>
#include <sys/select.h>

#include "ad1.h"
#include "helpmsgs.h"
#include "y.tab.h"

#define MEMFILE "/opt/pidp1-mods/coremem"

int exitReason;         // EXIT or QUIT, tells leave() how to treat breakpoints, 0 means an abnormal exit.

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
    {"format", 2, FORMAT, formatHelp},
    {"help", 1, HELP, NIL},
    {"list", 1, LIST, listHelp},
    {"load", 2, LOAD, loadHelp},
    {"monitor", 3, MONITOR, monitorHelp},
    {"next", 1, NEXT, nextHelp},
    {"quit", 1, QUIT, exitHelp},
    {"set", 2, SET, setHelp},
    {"show", 2, SHOW, showHelp},
    {"start", 3, START, startHelp},
    {"stop", 3, STOP, stopHelp},
    {"step", 1, STEP, stepHelp},
    {"trace", 1, TRACE, traceHelp},
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
    {"multiplefiles", 1, 0, multifileHelp},
    {0,0,0}
    };

int memListCount;
void *memList[256];     // allocated memory

static bool flexShifted = true; // last shift state returned by flexToAscii(), true is UC,  reset on format change

DispatchP findCommand(DispatchP tableP, char *nameP);

int isBreakpoint(int addr);
bool validateBreakpointNumber(int num);
void deleteAllBreakpoints(void);
void listBreaks();

bool validateWatchNumber(int num);
void deleteAllWatches(void);
void listWatches();

void sigHandler(int signo);
int getCurrentPC(void);
char *getFormat(int fmt);
char *getFormatName(int fmt);
char *getUnrestrictedFormat(int fmt);
void formatAndPrintOne(int fmt, int value);
void formatAndPrintTwo(int fmt1, int addr, int fmt2,  int value);
void printNumber(int format, int value);
void printAscii(char ch);
bool printHitText(u32 address);
bool printFlex(bool shifted, char ch);
bool loadMemoryFromFile(char *filenameP, Word memory[], Word memSize);
void usage(void);

static void leave(int, void *);
static bool serviceEvents(void);

extern int base;       // current number base
extern int lastFormat;  // the last format type used
extern int curStartAddr;     // set by the start or load commands
extern int curBank;     // set by the bank cmd
extern int curFileNo;   // which file we are using
extern int curLine;     // which file we are using

extern int yydebug;
extern int yy_flex_debug;
extern int lastAddr;
extern char *am1NameP;
extern char *lstNameP;
extern char *symNameP;

extern int parseAndExecute(char *lineP);
extern int getMapForFileNo(MapEntryP mapP, int fileNo);
extern int signExtend(int oc);
extern int twosCompl(int val);
extern int getNumber(char *strP, int base);
extern int flexToAscii(int ch, bool *shiftP);
extern bool isFileMapped(int fileno);
extern bool printLine(int fileNo, int lineNo);
extern bool printNextLine(void);
extern char *findNameByAddr(u32 addr);
extern char *decodeInstr(int word, int addr, bool asMacro, char *separatorP, char *symbolP, char *resultP, int *flagsP);
extern bool loadFileMap(bool fromLst, char *filenameP);
extern void closeFiles(void);
extern void listFn(int arg, MapEntryP mapP);
extern MapEntryP getLinesFromAddress(int addr);
extern FileInfoP newFile(char *nameP);
extern FileInfoP getFileInfoP(int fileNo);

int
main(int argc, char **argv)
{
int i;
int inFd;
int linkFd;
int maxFd;
int exitStatus;
bool testMode;
char *cP;
char *hostP;            // -h host[:port], NIL means the emulator on this machine
FileInfoP infoP;
fd_set read_fds;
char line[256];

    yy_flex_debug = yydebug = 0;
    symFileNameP = NIL;
    testMode = false;
    hostP = NIL;

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

            case 'h':
                // The value is the rest of this argument or the whole next one.
                if( *cP )
                {
                    hostP = cP;
                    cP += strlen(cP);
                }
                else if( argc > 1 )
                {
                    --argc;
                    ++argv;
                    hostP = *argv;
                }
                else
                {
                    usage();
                }
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

    curFileNo = -1;
    while( argc-- >= 1 )
    {
        if( (infoP = newFile(*(argv++))) )
        {
            // The first file is the default
            if( curFileNo == -1 )
            {
                curFileNo = infoP->fileNo;
            }
        }
    }

    curStartAddr = -1;
    curLine = -1;

    inFd = STDIN_FILENO;            // File descriptor for standard input

    // The command loop selects on stdin and the link together, so a line the user has already
    // typed must not sit hidden in stdio's buffer where select() cannot see it.
    setvbuf(stdin, NULL, _IONBF, 0);

    if( testMode )
    {
        // A local image with no emulator behind it, for standalone testing.
        tgtOpenLocal();
        if( !loadMemoryFromFile(MEMFILE, tgtLocalCore(), MAXMEM) )
        {
            printf("Can't load memory image from file '%s', memory may be empty.", MEMFILE);
        }
    }
    else
    {
        if( tgtOpen(hostP) != 0 )
        {
            fprintf(stderr, "%s\n", ad1LinkError());
            exit(1);
        }

        // Whatever kills us, from the kill command to a lost network, the emulator should drop our
        // breakpoints and watches. The exit command changes this to disable-only just before it
        // closes.
        tgtSetPolicy(AD1P_POLICY_DELETE_ALL);
    }

    // Now we want to be sure we exit cleanly, even if interrupted
    signal(SIGINT, sigHandler);
    signal(SIGTERM, sigHandler);

    on_exit(leave, 0);
    base = OCTAL;               // default is octal
    lastFormat = OCTAL;
    printf("Type 'help' for help.\n");

    while( true )
    {
        // Hits that arrived while a command was running are reported before the prompt.
        // Flushed here because the prompt below goes out by write(), ahead of anything buffered.
        tgtPump();
        if( serviceEvents() )
        {
            fflush(stdout);
        }

        write(STDOUT_FILENO, "Cmd? ", 5);   // this silliness to get around buffering issues with select

        while( true )
        {
            FD_ZERO(&read_fds);
            FD_SET(inFd, &read_fds);
            maxFd = inFd;
            if( (linkFd = tgtFd()) >= 0 )
            {
                FD_SET(linkFd, &read_fds);
                if( linkFd > maxFd )
                {
                    maxFd = linkFd;
                }
            }

            if( (i = select(maxFd + 1, &read_fds, NULL, NULL, NULL)) < 0 )
            {
                if( errno == EINTR )
                {
                    continue;
                }

                printf("Error in select(), terminating,\n");
                exit(1);
            }

            if( (linkFd >= 0) && FD_ISSET(linkFd, &read_fds) )
            {
                tgtPump();              // ends the program if the emulator has gone away
                if( serviceEvents() )
                {
                    fflush(stdout);         // nothing else will flush it until the next line is typed
                    write(STDOUT_FILENO, "Cmd? ", 5);
                }
            }

            if( FD_ISSET(inFd, &read_fds) )
            {
                break;                  // input ready
            }
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
        if( cP )
        {
            *cP = NUL;
        }

        exitStatus = parseAndExecute(line);
        if( (exitStatus == EXIT) || (exitStatus == QUIT) )
        {
            break;          // exit done
        }
    }

    exitReason = exitStatus;
    exit(0);
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

// Convert a format number to a printable string.
char *
getFormatName(int fmt)
{
    switch( fmt )
    {
    case 0:
        return("automatic, determined by 0n, n, 0bn, 0xn");
    case BINARY:
        return("b - binary");
    case OCTAL:
        return("o - octal");
    case DECIMAL:
        return("d - decimal");
    case HEX:
        return("x - hexadecimal");
    case ONESCMPL:
        return("c - ones complement");
    case ASCII:
        return("a - ascii");
    case FLEX:
        return("f - flex");
    case SYMBOLIC:
        return("s - symbolic");
    case INSTRUCTION:
        return("i - instruction");
    default:
        return("unknown - internal error");
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
            printNumber(lastFormat, value);
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
        addr = FULLADDR(curBank, value);
        cP = findNameByAddr(addr);
        decodeInstr(value, ADDRESSOF(value), false, " ", cP, tmpstr, 0);
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
    else if( fmt == NONE )
    {
        printNumber(lastFormat, value);
    }
    else
    {
        printNumber(fmt, value);
    }
}

// Print a number using the current base.
void
printNumber(int format, int value)
{
    printf(getFormat(format), value);
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
// If so, return its number, else 0.
int
isBreakpoint(int addr)
{
int i;
Ad1BpEntry bps[AD1_NUM_BREAKPOINTS];

    if( tgtBpList(bps) != AD1P_ST_OK )
    {
        return(0);
    }

    for( i = 0; i < AD1_NUM_BREAKPOINTS; ++i )
    {
        if( bps[i].isSet && (bps[i].address == (uint32_t)(addr & 0177777)) )
        {
            return( (int)bps[i].number );
        }
    }

    return(0);
}

// Does what is says.
void
deleteAllBreakpoints()
{
    tgtBpDelete(0);
}

void
listBreaks()
{
int i;
int nSet;
Ad1BpEntry bps[AD1_NUM_BREAKPOINTS];

    nSet = 0;
    if( tgtBpList(bps) == AD1P_ST_OK )
    {
        for( i = 0; i < AD1_NUM_BREAKPOINTS; ++i )
        {
            if( bps[i].isSet )
            {
                ++nSet;
                printf("%d: ", bps[i].number);
                formatAndPrintOne(SYMBOLIC, bps[i].address);
                printf(" ,count %d ,currently %d,", bps[i].count, bps[i].curCount);
                printf(" %s\n", (bps[i].isEnabled)?"enabled":"disabled");
            }
        }
    }

    if( nSet == 0 )
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

// Does what is says.
void
deleteAllWatches()
{
    tgtWatchDelete(0);
}

void
listWatches()
{
int i;
int nSet;
Ad1WatchEntry watches[AD1_NUM_WATCHES];

    nSet = 0;
    if( tgtWatchList(watches) == AD1P_ST_OK )
    {
        for( i = 0; i < AD1_NUM_WATCHES; ++i )
        {
            if( watches[i].isSet )
            {
                ++nSet;
                printf("%d: address ", watches[i].number);
                formatAndPrintOne(SYMBOLIC, watches[i].address);
                if( watches[i].onAnyChange )
                {
                    printf(" any value");
                }
                else
                {
                    printf(" value %06o", watches[i].value);
                }
                printf(" %s\n", (watches[i].isEnabled)?"enabled":"disabled");
            }
        }
    }

    if( nSet == 0 )
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

// Return the current full address of the pc in the emulator, extension bits included.
int
getCurrentPC()
{
uint32_t state[AD1P_STATE_WORDS];

    tgtGetState(state);
    return( (int)state[AD1P_STATE_PC] );
}

// Print a breakpoint or watch hit the emulator reported, and tell it we have seen it so the
// latch clears.
static void
reportHit(const Ad1Event *evP)
{
int number;
int address;
int word;
char *cP;
char line[256];

    if( (evP->type != AD1P_EVT_HIT_BREAK) && (evP->type != AD1P_EVT_HIT_WATCH) )
    {
        return;
    }

    number = (int)evP->words[0];
    address = (int)evP->words[1];
    word = (int)evP->words[2];      // what is in memory at the address now

    printf("\n%s %d hit", (evP->type == AD1P_EVT_HIT_BREAK)?"Breakpoint":"Watch", number);
    lastAddr = address;
    if( !printHitText(address) )
    {
        cP = findNameByAddr(address);
        decodeInstr(word, ADDRESSOF(word), false, " ", cP, line, 0);
        printf(": %s\n", line);
    }

    tgtAckHit((evP->type == AD1P_EVT_HIT_BREAK)?AD1P_HIT_BREAK:AD1P_HIT_WATCH);
}

// Report every hit queued so far. Returns true if there were any.
static bool
serviceEvents(void)
{
Ad1Event event;
bool any;

    any = false;
    while( tgtNextEvent(&event) )
    {
        reportHit(&event);
        any = true;
    }

    return( any );
}

// Print the source lines at a location, used to report breapoint and watchpoint hits.
// Return true if the lines were printed, else false.
bool
printHitText(u32 address)
{
MapEntryP mapP;
FileInfoP infoP;

    if( (mapP = getLinesFromAddress(address)) )
    {
        curFileNo = mapP->fileNo;

        if( (infoP = getFileInfoP(curFileNo)) )
        {
            printf(" at line %d,file %s:\n", mapP->lineNo, infoP->am1NameP);
            if( printLine(mapP->fileNo, mapP->lineNo) )
            {
                NEWLINE;
                return(true);
            }
        }
    }

    return(false);
}

// The exit status is not the reason we are leaving, main does exit(0), so use exitReason.
void
leave(int status, void *ignore)
{
    // The emulator drops the sticky single-step state itself when we disconnect. Exit preserves
    // all the breakpoints but disables them, anything else deletes them.
    tgtClose( (exitReason == EXIT)?AD1P_POLICY_DISABLE_ALL:AD1P_POLICY_DELETE_ALL );
    closeFiles();
}

// The emulator applies the delete-all policy set at startup when the connection closes.
void
sigHandler(int signo)
{
    _exit(1);
}

void
usage()
{
    printf("Usage: ad1 [-v] [-y] [-x] [-T] [-h host[:port]] [filename ...]\n");
    printf("-v prints the version and exits\n");
    printf("-y enables yacc debugging\n");
    printf("-x enables lex debugging\n");
    printf("-T enables test mode\n");
    printf("-h connects to the emulator on another machine, default is this one\n");
    exit(1);
}
