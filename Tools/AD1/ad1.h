// Primary include file for ad1

#define VERSION "1.0 28-Feb-2026"

#include <stdint.h>

#define NIL (void *)0
#define NUL (char)0
#define BADNUM -0xFFFFFF    // signal an invalid number, this can never be valid
#define NOARG -1            // signal no argument, used in yacc and ad1.c
#define NEWLINE fputc('\n', stdout)
#define PRINTCH(c) fputc(c, stdout)

#define MAXBANKS 16

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

// Define bases, values match what strtol() wants.
#define AUTOBASE 0
#define BINARY 2
#define OCTAL 8
#define DECIMAL 10
#define HEX 16
#define SYMBOLIC 100    // special marker for symbolic printing
#define ASCII 101       // special marker for ascii char printing
#define FLEX 102        //  special marker for flex/concise char printing
#define ONESCMPL 103    // special marker for ones complement printing
#define ADDR 104        // special marker for addresses, tries SYMBOLIC, if not uses address formatting

typedef uint32_t u32;

// Nothing fancy, just a linear list. Won't be enough symbols to need otherwise.
typedef struct {
    u32 address;
    char *nameP;
    int lineno;
    } Symbol, *SymbolP;

// Definition of a command
typedef struct {
    char *nameP;
    int significant;
    int token;
    char **helpText;
    } Dispatch, *DispatchP;

// Used to pass argument info from eval to yacc
typedef struct arg_s {
    struct arg_s *nextP;
    int value;
    } Arg, *ArgP;
