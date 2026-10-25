// Includes for the am1 PDP-1 assembler

#ifndef AM1VERSION

#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>

#include "symtab.h"

#define AM1VERSION "am1 v5.4 25-Oct-2026"
#define AM1SHORTVERSION "am1 v5.4"
#define SYMFILEVERSION "V3"             // used for import to check proper version, must match listSymtab.c

#define AM1INCDIR "/opt/pidp1-mods/Am1Includes"

#define WRDMASK 0777777 // PDP-1 word, 18 bits
#define ADDRMASK 07777  // PDP-1 address, 12 bits
#define MAXBANK 15      // 16 banks
#define BANKSIZE 4096   // of 4096 words

// Overlays
#define DRUMTRACKS 32           // tracks on the Type 23 drum, 0-37 octal
#define OVHEADERWORDS 2         // the stamp and the overlay's number, at its window's start
#define OVENTRYWORDS 4          // descriptor table words per overlay
#define OVTABLENAME "ovTable"   // the label am1 gives the descriptor table
#define OVENTERNAME "ovEnter"   // the overlay manager's entry, which %%ovcall calls
#define OVFETCHNAME "ovFetch"   // the overlay manager's entry, which %%ovload calls

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
#define WARN_T340 11
#define WARN_OVERLAY 12

// actually the max local scope nesting
#define MAXLOCALS   128

// PNode flags
#define PN_NOINC    1   // don't increment pc
#define PN_SOL      2   // used to signal a comment at the beginning of a line
#define PN_NOTEXT   4   // used to signal no included text in listing file
#define PN_HANDSOFF 8   // an OPTIMIZE or ENDOPTIMIZE node written as nooptimize or
                        // endnooptimize: a span no optimization level rewrites.
                        // Same node type, so every back end steps over it
#define PN_SPEED    16  // an OPTIMIZE or ENDOPTIMIZE node written as speed or
                        // endspeed: a span where words may be spent, a separate
                        // claim from a region's "P5 holds here"
#define PN_INLINE   32  // an OPTIMIZE node written as 'inline NAME': a point marking
                        // a routine that may be inlined wherever it is called;
                        // value2.strP holds the name
#define PN_CEILING  64  // an OPTIMIZE node written as '%%ceiling EXPR', a point;
                        // value2.ival holds the ceiling reduced to the bank, and
                        // bank the bank
#define PN_OVLOAD   128 // an OVCALL node written as %%ovload
#define PN_OVRES    256 // an OVCALL node written as %%ovresident, which names no entry

// The directive an OVCALL node was written as, without its %%.
#define OVCALLNAME(nodeP) ((((nodeP)->flags) & PN_OVRES)? "ovresident": \
                           (((nodeP)->flags) & PN_OVLOAD)? "ovload": "ovcall")

// What an OVCALL node names after its overlay, the comma and entry, nothing for %%ovresident.
#define OVCALLSEP(nodeP) ((((nodeP)->flags) & PN_OVRES)? "": ", ")
#define OVCALLENTRY(nodeP) ((((nodeP)->flags) & PN_OVRES)? "": (nodeP)->value2.strP)

// am1's own names for an %%ovresident's two numbers, the overlay's name and one of these:
// its window's word's place in ovTable and its number.
// A dot can't be in a symbol, so no program can write either.
#define OVRESWINDOW ".window"
#define OVRESNUMBER ".number"

// What a RELAYOUT node is, in its value.ival.
// Only optrelayout.c makes them, every back end but the listing skips them
#define RL_DELETED  1   // a word deleted, exprP holds its expression, value2.ival is
                        // non-zero when the word shared a line with a label
#define RL_MOVEDFROM 2  // where a moved run was, value2.ptr is its RL_BEGIN
#define RL_BEGIN    3   // the moved run follows, value2.ptr is its RL_END
#define RL_END      4   // the moved run ended

// Symbol table flags and such
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
#define SYMF_OVALIAS 0x800000   // an overlay's name for its bank's root global of
                                // that name, symP is the root's symbol
#define SYMF_OVNUMBER 0x1000000 // an %%ovresident's number, ov.window or ov.number:
                                // a value, not the address of a word

#define CTX_FORCELOCAL 1        // forcelocal is active for this context

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
    struct parsenode *exprP;    // an expression relayout must evaluate again or on a
                                // RELAYOUT node the deleted word's, NILP elsewhere
    int srcLine;                // on a BREF made from a name, the line and file it
    char *srcFileP;             // was written on, for the error given after the
                                // parse if its symbol never resolves, and on an
                                // OVCALL its own.
                                // 0 and NILP elsewhere
    int overlay;                // the number of the overlay the node is in, 0 for the root
    const struct PPOrigin *originP; // the macros that made its statement, NULL for none
} PNode, *PNodeP;

// The origin of the statement the lexer is reading, from preprocess.c, which
// newnode() gives each node; NULL when no macro made it.
extern const struct PPOrigin *stmtOriginP;

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
    int runPC;              // where the current run of words began, at the last origin
    int highPC;             // one past the highest word of the runs before it, 0 for none
} BankContext, *BankContextP;

// A core window that overlays run in: a range of one bank, declared by %%window,
// which no root word may use.
typedef struct window
{
    struct window *nextP;   // in declaration order
    char *nameP;
    int bank;
    int first;              // its first and last address in the bank
    int last;
    int index;              // 0 up, in declaration order
    int lineNo;             // where it was declared
    char *fileP;
    bool twopass;           // a persistent overlay is written back in a transfer of its
                            // own, not swapped, so its overlays are placed one by one
} Window, *WindowP;

// An overlay.
// A section of the program assembled to run in a window, the code excluded from the
// the man program's tape.
// While it is open, curBankP points at ctx.
// Every grammar action places words, labels, constants and variables in the
// overlay's own context, not its bank's.
typedef struct overlaycontext
{
    struct overlaycontext *nextP;   // in source order
    char *nameP;
    int number;             // 1 up, in source order; 0 means the root
    WindowP windowP;
    bool persistent;        // written back to the drum when evicted
    BankContext ctx;        // its location counter, symbols, constants and
                            // variables.
                            // ctx.bank is the window's bank,
                            // ctx.constPC and ctx.varPC where %%endoverlay put
                            // its pools, -1 for none
    int lineNo;             // its %%overlay line, and file
    char *fileP;
    // The overlay words, header included, and their place on the drum.
    // ovlcodegen.c sets these after the parse.
    int length;
    Word *imageP;
    int track;              // drum track they go on
    int drum;               // the drum address of the span's first word
    int span;               // the words of the track that are the overlay's own: its
                            // length, or in a swap window, for a persistent overlay,
                            // the window's longest overlay's
} OverlayContext, *OverlayContextP;

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
