// Includes for the am1 PDP-1 assembler

#ifndef AM1INCDIR

#include <stdlib.h>

#include "symtab.h"

#define AM1VERSION "am1 v1.0 4-Dec-2025"
#define AM1INCDIR "/opt/pidp1/MacroIncludes"

#ifndef CPP_PATH
#define CPP_PATH    "/usr/bin/cpp"
#endif

#define WRDMASK 0777777 // PDP-1 word, 18 bits

// actually the max local scope nesting
#define MAXLOCALS   128

// PNode flags
#define PN_NOINC    1   // don't increment pc

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

#define CTX_FORCELOCAL 1        // focelocal is active for this context

#define NILP 0

// Markers used in asciiToFlexo conversion
#define NONE        -1
#define CUNSHIFT   072
#define CSHIFT     074
#define FLEX_SPACE 000

// Our internal parse tree node and values
typedef union
{
    int ival;
    char *strP;
    SymNodeP symP;      // symbol node
} PNodeValue;

typedef struct parsenode
{
    struct parsenode *leftP;
    struct parsenode *rightP;
    int type;               // node type
    int flags;              // user defined flags
    int pc;                 // the mem loc for this node, if any
    PNodeValue value;
    PNodeValue value2;
} PNode, *PNodeP;

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
    SymNodeP globalSymP;    // we preserve the globals and consts, no need for locals, will never be in process
    SymNodeP constSymP;
} BankContext, *BankContextP;

#endif
