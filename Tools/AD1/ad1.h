// Primary include file for ad1

#define VERSION "1.7 13-Mar-2026"

#include <stdint.h>

#define NIL (void *)0
#define NUL (char)0
#define BADNUM -0xFFFFFF    // signal an invalid number, this can never be valid
#define NOARG -1            // signal no argument, used in yacc and ad1.c
#define NEWLINE fputc('\n', stdout)
#define PRINTCH(c) fputc(c, stdout)

#define MAXBANKS 16

#define BANKOF(x) (((x) >> 12) & 017)
#define ADDRESSOF(x) ((x) & 07777)

// flexToAscii() returns
#define NOCHAR -1
#define LCS -2
#define UCS -3

// Flags used in Directives
#define FORCE_DECIMAL 1 // a number will only be interpreted as a base 10 number
#define FORCE_STRING 2  // an alphanumeric string will be returned only as a string
#define FORCE_RAW 3     // no processing of a token is done, the full text is returned as a string

// Define registers
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

// Nothing fancy, just a linear list. Won't be enough symbols to need otherwise.
typedef struct {
    u32 address;
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
typedef struct MapEntry_t
{
    short lineNo;
    struct MapEntry_t *nextP;
} MapEntry, *MapEntryP, **MapEntryPP;
