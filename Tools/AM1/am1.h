// Includes for the am1 PDP-1 assembler

#ifndef AM1VERSION

#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>

#include "symtab.h"

#define AM1VERSION "am1 v1.46 5-Sep-2026"
#define AM1SHORTVERSION "am1 v1.46"
#define SYMFILEVERSION "V2"             // used for import to check proper version

#define AM1INCDIR "/opt/pidp1-mods/Am1Includes"

#ifndef CPP_PATH
#define CPP_PATH    "/usr/bin/cpp"
#endif

#define WRDMASK 0777777 // PDP-1 word, 18 bits
#define ADDRMASK 07777  // PDP-1 address, 12 bits
#define MAXBANK 15      // 16 banks
#define BANKSIZE 4096   // of 4096 words

// The warning ids we have
#define WARN_1D 1
#define WARN_BANKS 2
#define WARN_LOCALS 3
#define WARN_FLEX 4
#define WARN_VARS 5
#define WARN_STOP 6
#define WARN_BANK 7
#define WARN_BREF 8
#define WARN_MEMORY 9
#define WARN_LAW 10

// actually the max local scope nesting
#define MAXLOCALS   128

// PNode flags
#define PN_NOINC    1   // don't increment pc
#define PN_SOL      2   // used to signal a comment at the beginning of a line
#define PN_NOTEXT   4   // used to signal no included text in listing file

// Symbol table flags amd such
#define SYM_VALUE 1
#define SYM_OPCODE 2
#define SYM_OPADDR 3
#define SYM_OPORABLE 4
#define SYM_LOC 5
#define SYM_GLOB 6

#define SYM_MASK 0xFF
#define SYMF_PERM 0x100
#define SYMF_RESOLVED 0x200     // has been resolved to its final value
#define SYMF_VAR 0x400          // is a variable
#define SYMF_FORCED 0x1000      // is a forced-local from a local context
#define SYMF_ASSIGNED 0x2000    // is a constant that has been assigned a location
#define SYMF_EVALED 0x4000      // is a constant that has been evaluated
#define SYMF_EMITTED 0x8000     // is a constant that has been emitted
#define SYMF_IMPORTED 0x10000   // came from an import file
#define SYMF_EXPORTED 0x20000   // needs to be exported
#define SYMF_1DOP 0x40000       // is a PDP-1D added instruction
#define SYMF_LAW 0x100000       // is a law instruction, special case
#define SYMF_PRIVATE 0x200000   // is a private local
#define SYMF_INDIRECT 0x400000  // is an i modifier

#define CTX_FORCELOCAL 1        // focelocal is active for this context

#define NILP 0

// Markers used in asciiToFlexo conversion
#define NONE        -1
#define CUNSHIFT   072
#define CSHIFT     074
#define RED        035
#define BLACK      034
#define FLEX_SPACE 000
#define FLEX_CR    077

typedef uint32_t Word;         // minimum needed to hold a PDP-1 word, 18 bits

// holder for flex characters, can contain null
typedef struct flextext
{
    int nchars;             // total chars in bufP
    char *bufP;             // and where the chars are
} FlexText, *FlexTextP;

// Our internal parse tree node and values
typedef union
{
    int ival;
    char *strP;
    SymNodeP symP;      // symbol node
    void *ptr;
    FlexText flexText;
} PNodeValue;

typedef struct parsenode
{
    struct parsenode *leftP;
    struct parsenode *rightP;
    int type;               // node type
    int flags;              // user defined flags
    int pc;                 // the mem loc for this node, if any
    int bank;               // and the bank it's in
    int lineNo;             // and the source line
    PNodeValue value;
    PNodeValue value2;
} PNode, *PNodeP;

// a list of PNodePs, used for wildcard cross-bank refs
typedef struct parsenodelist
{
    struct parsenodelist *nextP;
    PNodeP nodeP;
} PNodeListItem, *PNodeListP;

// Context for a local scope
typedef struct
{
    int flags;              // for forcelocal
    int pc;                 // the pc at the start of the local scope
    SymNodeP symRootP;      // the symbol table for the scope
} LocalContext, *LocalContextP;

// Context for a memory bank
typedef struct bankcontext
{
    struct bankcontext *nextP;
    int bank;               // the bank number
    int cur_pc;             // pc at the time of the switch from this bank
    int constPC;            // start of the automatically emitted constants block, -1 if none
    int varPC;              // start of the automatically emitted variables block, -1 if none
    SymNodeP globalSymP;    // we preserve the globals,consts, and vars, no need for locals
    SymNodeP constSymP;
    PNodeListP varNodesP;
} BankContext, *BankContextP;

// list of symtabs
typedef struct symlist
{
    SymNodeP symP;
    int bank;               // the bank this was defined in
    int pc;                 // the PC for the first location of this list
    struct symlist *nextP;
} SymList, *SymListP;

// Define the various warnings that can be enabled and disabled
typedef struct {
    char name[16];
    int id;
    bool enabled;
    bool repeats;
    bool issued;
} Warning, *WarningP;
#endif
