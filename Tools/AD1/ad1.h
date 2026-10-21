// Primary include file for ad1
#include <stdint.h>
#include <stdio.h>

#include "ad1link.h"
#include "target.h"

#define VERSION "2.8 21-Oct-2026"

#define MAXFILES    8    // maximum number of open files we can have
#define MAXLINES    10000 // max number of lines in a file

#define MEMBANKS    16
#define MEMSIZE     4096

#define NIL (void *)0
#define NUL (char)0
#define BADNUM -0xFFFFFF    // signal an invalid number, this can never be valid
#define NOARG -1            // signal no argument, used in yacc and ad1.c
#define NEWLINE fputc('\n', stdout)
#define PRINTCH(c) fputc(c, stdout)

#define BANKOF(x) (((x) >> 12) & 017)
#define ADDRESSOF(x) ((x) & 07777)
#define FULLADDR(bank, addr) (((bank) << 12) | ADDRESSOF(addr))

#define INDIRECT_BIT 010000

// flexoToAsciiShift() returns
#define NOCHAR -1
#define LCS -2
#define UCS -3

// loader returns
#define LOADFAILED -1
#define LOADSTOP -2

// The drum file load writes a program's overlays to, unless -i names another.
#define DRUMFILE "/opt/pidp1-mods/pdp23drum"
extern char *drumFileP;

// Flags used in Directives
#define FORCE_DECIMAL 1 // a number will only be interpreted as a base 10 number
#define FORCE_STRING 2  // an alphanumeric string will be returned only as a string
#define FORCE_RAW 3     // no processing of a token is done, the full text is returned as a string

// Define registers
#define NOREG 0         // a marker used for the show last command
#define ACREG 1
#define IOREG 2
#define PCREG 3
#define TWREG 4
#define MAREG 5
#define MBREG 6
#define SSREG 7
#define PF1REG 8    // keep these in consecutive value order
#define PF2REG 9
#define PF3REG 10
#define PF4REG 11
#define PF5REG 12
#define PF6REG 13
#define PFREG 14        // group reference to them all
#define ASREG 15
#define SYREG 16

// Define bases, values match what strtol() wants.
#define NONE -1
#define AUTOBASE 0
#define BINARY 2
#define OCTAL 8
#define DECIMAL 10
#define HEX 16
#define SYMBOLIC 100      // marker for symbolic printing
#define INSTRUCTION 101   // marker for instruction printing
#define ASCII 102         // marker for ascii char printing
#define FLEX 103          // marker for flex/concise char printing
#define ONESCMPL 104      // marker for ones complement printing
#define ADDRESS 105       // marker for symbolic, use base 8 if no symbol

typedef uint32_t u32;
typedef uint32_t Word;    // an 18-bit PDP-1 word held in 32 bits

// Sizes come from the link protocol, so ad1 follows whatever the emulator was built with.
#define MAXMEM AD1P_MEM_WORDS
#define AD1_NUM_BREAKPOINTS AD1P_NUM_BREAKPOINTS
#define AD1_NUM_WATCHES AD1P_NUM_WATCHES

// Nothing fancy, just a linear list. Won't be enough symbols to need otherwise.
typedef struct {
    u32 address;        // full 16 bit address
    int fileNo;
    int overlay;        // the overlay's number in its file, 0 for the root
    char *nameP;
    } Symbol, *SymbolP;

// Definition of a command
typedef struct {
    char *nameP;
    short significant;
    short token;
    char **helpText;
    } Dispatch, *DispatchP;

// Used to pass argument info from eval to yacc
typedef struct arg_s {
    struct arg_s *nextP;
    int value;
    } Arg, *ArgP;

// For the address to line number(s) mapping
typedef struct MapEntry_t {
    short fileNo;               // internal number assigned to file, index in files table
    short lineNo;               // line number in file
    short overlay;              // the listing line's overlay, 0 for the root
    struct MapEntry_t *nextP;
} MapEntry, *MapEntryP, **MapEntryPP;

// A word's origin, from am1's .map: one level per macro expansion that created its statement,
// from the macro the program used inward, each with the body line it came from.
typedef struct {
    char *macroP;
    char *fileP;                // as the .map names it, NIL if not known
    int lineNo;                 // the body line in fileP
} OriginLevel, *OriginLevelP;

typedef struct {
    int count;
    OriginLevelP levelsP;
} Origin, *OriginP;

// For file information
typedef struct {
    int fileNo;                 // index in files table
    char *am1NameP;
    char *lstNameP;
    char *symNameP;
    FILE *fP;
    int numLines;               // lines in the file
    long lineMap[MAXLINES];     // map line numbers in file into file offsets in file
    OriginP *originsPP;         // by listing line, the macro chain from the .map.
                                // NIL for a table without a .map, NIL for a line with no chain
    char **mapFilesPP;          // the .map's file names, by its number for them
    int numMapFiles;
} FileInfo, *FileInfoP;

void ovlLoad(FileInfoP infoP);
int ovlSymbolsFrom(const char *pathP);
void ovlClear(void);
void ovlForget(void);
bool ovlSymbolShown(int fileNo, int overlay);
bool ovlLineShown(int fileNo, int overlay);
int ovlNumber(int fileNo, char *nameP);
char *ovlName(int fileNo, int overlay);
char *ovlResidentNameAt(u32 address);

// An address named with name@overlay which break binds its breakpoint to.
typedef struct {
    int count;                  // name@overlay references in the command, 0 for none
    bool mixed;                 // they name more than one overlay
    int fileNo;                 // the first one's
    int overlay;
    u32 address;
    char *nameP;
} OverlayRef, *OverlayRefP;

void ovlRefClear(void);
void ovlRefNote(SymbolP symP);
OverlayRefP ovlRef(void);

// Breakpoints and watches bound to an overlay.
typedef struct {
    bool fires[AD1_NUM_BREAKPOINTS];    // by number less one, the bound ones that fire
    int unfired;                // a bound breakpoint at the address that did not fire, 0 if none
    bool said;                  // a message was printed
    char why[160];              // why unfired did not fire
} BoundHit, *BoundHitP;

const char *ovlCondFor(OverlayRefP refP, Ad1Cond *condP);     // nil, or why there is none
void ovlBindBreak(int number, OverlayRefP refP, int count, bool inEmu);
void ovlUnbindBreak(int number);          // 0 for every one
bool ovlPrintBinding(int number, u32 address, bool enabled, int count, int curCount);
bool ovlBreakHit(int number, u32 address, BoundHitP hitP);
void ovlBindWatch(int number, OverlayRefP refP);
void ovlUnbindWatch(int number);          // 0 for every one
bool ovlPrintWatchAddress(int number, u32 address);
char *ovlWatchOverlay(int number);
bool ovlWatchHit(int number, bool *saidP);
void ovlBreaksClosed(void);
void ovlBreaksLeft(void);

// Set by a step or monitor that ended on a hit, so the hit's report knows it was a step's.
extern bool stepEndedOnHit;

// Symbols, eval.c, for the overlays' symbols.
bool addSymbol(u32 address, int fileNo, int overlay, char *nameP);
void clearOverlaySymbols(int fileNo);     // NOARG for every file
SymbolP findOverlaySymbol(char *nameP, char *ovNameP);
extern bool symReported;    // a lookup that failed has already said why
