/*
 * The am1 optimizer's reference analysis: the edges between words with the
 * role each reference plays, the reverse index, the written/patched/taken
 * flags, one-step pointer following, the code/data classification and the
 * words those two judgments disagree about.  Also the reference dump
 * (-O=refs) and the report's per-bank reference lines.
 *
 * Runs single threaded from optimize() in optimizer.c, after yyparse() and
 * before any code generator; it reads the parse tree and the resolved symbol
 * tables (am1.h, y.tab.h) and never modifies either.  Everything it derives
 * lives in the OptTable of optimizer.h.  Allocation failure is fatal, as
 * everywhere in am1.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>

#include "am1.h"
#include "y.tab.h"
#include "optimizer.h"

// The analysis makes five passes over the decoded table:
//
//   1. Direct edges.  A memory reference instruction (decoded as one AND
//      spelled with exactly one address-taking mnemonic) gets an edge for its
//      address field to the decoded address, in the mnemonic's role, carrying
//      the field's symbol if the field is a single symbol.  Every other
//      symbol leaf, in any word, is a "taken" edge to the symbol's own word.
//      jda and cal add the edges the hardware makes without being told.
//   2. Direct flags, from the in edges.  Then the placeholders are marked: an
//      edge from a patched word whose field was not a symbol ("rtn, jmp 0",
//      "dac 0") is the value before patching, never what runs; it is kept
//      for the dump but neither followed nor used to classify.  The flags are
//      derived again without them, so pass 3 can ask whether a pointer word
//      is ever written.
//   3. Indirect edges.  An i-bit edge goes to the pointer word.  If that word
//      is data, never written and holds a resolved address, a via-pointer
//      edge goes to that address; otherwise every taken word of the
//      referencing bank is marked possibly reached in the edge's role.
//   4. Final flags, from every edge including the via-pointer ones.
//   5. Classification and conflicts (classifyWords()).

// What is known about one memory reference mnemonic, indexed by the
// instruction code with bit 5 dropped (code >> 1), like memrefNames[].
// role is what the address field's target undergoes; secondRole a second
// edge to the same target (idx and isp both read and write), or OPTR_COUNT
// for none.  cal and jda (code 16) are not in the table; makeInstructionEdges()
// handles them by hand.
typedef struct
{
    OptRole role;
    OptRole secondRole;
    unsigned int flags;     // OPTEF_PATCH for dap and dip
} MemrefRole;

static const MemrefRole memrefRoles[32] =
{
    { OPTR_COUNT,   OPTR_COUNT, 0 },            // 00 spare
    { OPTR_READ,    OPTR_COUNT, 0 },            // 02 and
    { OPTR_READ,    OPTR_COUNT, 0 },            // 04 ior
    { OPTR_READ,    OPTR_COUNT, 0 },            // 06 xor
    { OPTR_EXECUTE, OPTR_COUNT, 0 },            // 10 xct
    { OPTR_COUNT,   OPTR_COUNT, 0 },            // 12 spare
    { OPTR_COUNT,   OPTR_COUNT, 0 },            // 14 spare
    { OPTR_COUNT,   OPTR_COUNT, 0 },            // 16 cal and jda: by hand
    { OPTR_READ,    OPTR_COUNT, 0 },            // 20 lac
    { OPTR_READ,    OPTR_COUNT, 0 },            // 22 lio
    { OPTR_WRITE,   OPTR_COUNT, 0 },            // 24 dac
    { OPTR_WRITE,   OPTR_COUNT, OPTEF_PATCH },  // 26 dap
    { OPTR_WRITE,   OPTR_COUNT, OPTEF_PATCH },  // 30 dip
    { OPTR_WRITE,   OPTR_COUNT, 0 },            // 32 dio
    { OPTR_WRITE,   OPTR_COUNT, 0 },            // 34 dzm
    { OPTR_COUNT,   OPTR_COUNT, 0 },            // 36 spare
    { OPTR_READ,    OPTR_COUNT, 0 },            // 40 add
    { OPTR_READ,    OPTR_COUNT, 0 },            // 42 sub
    { OPTR_READ,    OPTR_WRITE, 0 },            // 44 idx
    { OPTR_READ,    OPTR_WRITE, 0 },            // 46 isp
    { OPTR_READ,    OPTR_COUNT, 0 },            // 50 sad
    { OPTR_READ,    OPTR_COUNT, 0 },            // 52 sas
    { OPTR_READ,    OPTR_COUNT, 0 },            // 54 mul
    { OPTR_READ,    OPTR_COUNT, 0 },            // 56 div
    { OPTR_JUMP,    OPTR_COUNT, 0 },            // 60 jmp
    { OPTR_JUMP,    OPTR_COUNT, 0 },            // 62 jsp
    { OPTR_COUNT,   OPTR_COUNT, 0 },            // 64 skip group
    { OPTR_COUNT,   OPTR_COUNT, 0 },            // 66 shift group
    { OPTR_COUNT,   OPTR_COUNT, 0 },            // 70 law
    { OPTR_COUNT,   OPTR_COUNT, 0 },            // 72 iot
    { OPTR_COUNT,   OPTR_COUNT, 0 },            // 74 special operate
    { OPTR_COUNT,   OPTR_COUNT, 0 }             // 76 operate
};

// The flag names the dump prints, in order.
static const struct
{
    unsigned int bit;
    const char *nameP;
} wordFlagNames[] =
{
    { OPTF_CODE,            "code" },
    { OPTF_DATA,            "data" },
    { OPTF_START,           "start" },
    { OPTF_WRITTEN,         "written" },
    { OPTF_PATCHED,         "patched" },
    { OPTF_TAKEN,           "taken" },
    { OPTF_XCTTARGET,       "xct" },
    { OPTF_MAYBE_READ,      "maybe_read" },
    { OPTF_MAYBE_WRITTEN,   "maybe_written" },
    { OPTF_MAYBE_ENTERED,   "maybe_entered" }
};

static const struct
{
    unsigned int bit;
    const char *nameP;
} conflictNames[] =
{
    { OPTCF_CODE_DATA,      "code+data" },
    { OPTCF_CODE_PATCHED,   "code+patched" },
    { OPTCF_CODE_WRITTEN,   "code+written" }
};

static void makeDirectEdges(OptTableP tableP, OptWordP entryP);
static int isMemrefInstruction(OptWordP entryP);
static PNodeP fieldLeaf(PNodeP exprP);
static void countFieldLeaves(PNodeP nodeP, int inArith, int *countP, PNodeP *leafP);
static void makeLeafEdges(OptTableP tableP, OptWordP entryP, PNodeP nodeP, PNodeP fieldP);
static void makeInstructionEdges(OptTableP tableP, OptWordP entryP, PNodeP fieldP);
static void makeTakenEdge(OptTableP tableP, OptWordP entryP, PNodeP leafP);
static int leafTarget(OptTableP tableP, OptWordP entryP, PNodeP leafP, int *bankP, int *addrP);
static SymNodeP leafSymbol(PNodeP leafP);
static OptEdgeP addEdge(OptTableP tableP, OptWordP fromP, int bank, int addr, OptRole role,
    unsigned int flags, SymNodeP symP);
static OptEdgeP newEdge(OptTableP tableP, OptWordP fromP, OptWordP toP, int bank, int addr,
    OptRole role, unsigned int flags, SymNodeP symP);
static void deriveFlags(OptTableP tableP);
static void markPlaceholders(OptTableP tableP);
static void followPointers(OptTableP tableP);
static int followPointer(OptTableP tableP, OptWordP pointerP, OptWordP fromP, int *bankP, int *addrP);
static int pointerExprBank(PNodeP nodeP);
static int hasDirectWrite(OptWordP entryP);
static int hasPlainWrite(OptWordP entryP);
static void markConservatively(OptTableP tableP, int bank, OptRole role);
static void classifyWords(OptTableP tableP);
static void printEdge(FILE *fP, OptEdgeP edgeP, int outgoing);

// Build the references of a decoded table in the five passes above.
// Unresolved references are reported on stderr, counted and skipped.
void
optBuildReferences(OptTableP tableP)
{
int i;

    for( i = 0; i < tableP->count; ++i )
    {
        makeDirectEdges(tableP, tableP->entriesPP[i]);
    }

    deriveFlags(tableP);
    markPlaceholders(tableP);
    deriveFlags(tableP);
    followPointers(tableP);
    deriveFlags(tableP);
    classifyWords(tableP);
}

// Name a role.
// Returns a static string, never NILP.
const char *
optRoleName(OptRole role)
{
    switch( role )
    {
    case OPTR_READ:
        return("read");

    case OPTR_WRITE:
        return("write");

    case OPTR_JUMP:
        return("jump");

    case OPTR_EXECUTE:
        return("execute");

    case OPTR_TAKEN:
        return("taken");

    default:
        return("unknown");
    }
}

// Print the reference dump (-O=refs), one line per entry in emission order,
// reserved words included:
//
//   BBAAAA VVVVVV kind    FLAGS | out: EDGE, EDGE | in: EDGE, EDGE
//
// BBAAAA, VVVVVV and kind are as in the decoded dump (------ for a reserved
// word's value).  FLAGS is the word's flags in the order code data start
// written patched taken xct maybe_read maybe_written maybe_entered, then its
// conflicts as conflict:code+data, conflict:code+patched, conflict:code+written;
// "-" when there are none.  An out edge is
//
//   ROLE[ i][ via][ patch][ implicit][ crossbank][ unknown]->BBAAAA[?][ NAME]
//
// the role, the edge's flags in that order, the target (with ? when no word
// was emitted there), and the symbol the source wrote: its name, or [..] for
// a constant-pool reference.  An in edge is the same with <-BBAAAA naming the
// source and no symbol.  Either list is "-" when empty.
void
optDumpReferences(FILE *fP, OptTableP tableP)
{
int i;
int f;
int c;
int any;
OptWordP entryP;
OptEdgeP edgeP;

    for( i = 0; i < tableP->count; ++i )
    {
        entryP = tableP->entriesPP[i];

        fprintf(fP, "%02o%04o ", entryP->bank, entryP->addr);

        if( entryP->flags & OPTF_RESERVED )
        {
            fprintf(fP, "------ %-7s ", kindName(entryP->kind));
        }
        else
        {
            fprintf(fP, "%06o %-7s ", entryP->value, kindName(entryP->kind));
        }

        // The flags, then the conflicts.
        any = 0;

        for( f = 0; f < (int)(sizeof(wordFlagNames) / sizeof(wordFlagNames[0])); ++f )
        {
            if( entryP->flags & wordFlagNames[f].bit )
            {
                fprintf(fP, "%s%s", (any)?" ":"", wordFlagNames[f].nameP);
                any = 1;
            }
        }

        for( c = 0; c < (int)(sizeof(conflictNames) / sizeof(conflictNames[0])); ++c )
        {
            if( entryP->conflicts & conflictNames[c].bit )
            {
                fprintf(fP, "%sconflict:%s", (any)?" ":"", conflictNames[c].nameP);
                any = 1;
            }
        }

        if( !any )
        {
            fprintf(fP, "-");
        }

        // The out edges.
        fprintf(fP, " | out:");
        any = 0;

        for( edgeP = entryP->outP; edgeP; edgeP = edgeP->nextOutP )
        {
            fprintf(fP, (any)?", ":" ");
            printEdge(fP, edgeP, 1);
            any = 1;
        }

        if( !any )
        {
            fprintf(fP, " -");
        }

        // The in edges.
        fprintf(fP, " | in:");
        any = 0;

        for( edgeP = entryP->inP; edgeP; edgeP = edgeP->nextInP )
        {
            fprintf(fP, (any)?", ":" ");
            printEdge(fP, edgeP, 0);
            any = 1;
        }

        if( !any )
        {
            fprintf(fP, " -");
        }

        fprintf(fP, "\n");
    }
}

// Pass 1 for one word: make its direct edges.  A word with no expression
// (text, a reserved table word, an uninitialized variable) references nothing.
static void
makeDirectEdges(OptTableP tableP, OptWordP entryP)
{
PNodeP fieldP;

    if( (entryP->flags & OPTF_RESERVED) || !entryP->exprP )
    {
        return;
    }

    if( isMemrefInstruction(entryP) )
    {
        fieldP = fieldLeaf(entryP->exprP);
        makeInstructionEdges(tableP, entryP, fieldP);
        makeLeafEdges(tableP, entryP, entryP->exprP, fieldP);
    }
    else
    {
        // No address field: every symbol in the word is only a value.
        makeLeafEdges(tableP, entryP, entryP->exprP, NILP);
    }
}

// Is this word a memory reference instruction?  It must decode as one AND be
// spelled with exactly one address-taking mnemonic ("lac x", "cal", not
// "jmp+x" or 0200000): a data word that decodes as lac reads nothing.
// Returns 1 if it is, 0 if not.
static int
isMemrefInstruction(OptWordP entryP)
{
OptDecodeP decodeP;

    decodeP = &entryP->decode;

    if( decodeP->group != OPTG_MEMREF )
    {
        return(0);
    }

    if( (decodeP->opSymCount != 1) || !decodeP->opSymP ||
        ((decodeP->opSymP->flags & SYM_MASK) != SYM_OPADDR) )
    {
        return(0);
    }

    switch( decodeP->spelling )
    {
    case OPTS_MNEMONIC_OPERAND:
    case OPTS_MNEMONIC_ONLY:
    case OPTS_CONSTREF:
        return(1);

    default:
        return(0);
    }
}

// Find an instruction's address-field symbol: the only symbol leaf outside
// arithmetic ("lac x", "lac [x]"; "lac x+1" and "lac x y" have none).
// Returns the leaf node, or NILP when the field is not a single symbol.
static PNodeP
fieldLeaf(PNodeP exprP)
{
int count;
PNodeP leafP;

    count = 0;
    leafP = NILP;
    countFieldLeaves(exprP, 0, &count, &leafP);

    return( (count == 1)?leafP:NILP );
}

// Count the symbol leaves not inside arithmetic, remembering the last one.
// The separator and parentheses are transparent; a [..] is a leaf.
static void
countFieldLeaves(PNodeP nodeP, int inArith, int *countP, PNodeP *leafP)
{
    if( !nodeP )
    {
        return;
    }

    switch( nodeP->type )
    {
    case BINOP:
        if( nodeP->value.ival == SEPARATOR )
        {
            countFieldLeaves(nodeP->leftP, inArith, countP, leafP);
            countFieldLeaves(nodeP->rightP, inArith, countP, leafP);
        }
        else
        {
            countFieldLeaves(nodeP->leftP, 1, countP, leafP);
            countFieldLeaves(nodeP->rightP, 1, countP, leafP);
        }
        break;

    case UNOP:
        countFieldLeaves(nodeP->rightP, (nodeP->value.ival == PARENS)?inArith:1, countP, leafP);
        break;

    case ADDR:
    case LCLADDR:
    case BREF:
    case WILDREF:
    case CONSTANT:
        if( !inArith )
        {
            ++*countP;
            *leafP = nodeP;
        }
        break;

    default:
        break;
    }
}

// Make a taken edge for every symbol leaf other than the address field's
// own (fieldP, NILP when none), whose edge makeInstructionEdges() made.
static void
makeLeafEdges(OptTableP tableP, OptWordP entryP, PNodeP nodeP, PNodeP fieldP)
{
    if( !nodeP )
    {
        return;
    }

    switch( nodeP->type )
    {
    case BINOP:
        makeLeafEdges(tableP, entryP, nodeP->leftP, fieldP);
        makeLeafEdges(tableP, entryP, nodeP->rightP, fieldP);
        break;

    case UNOP:
        makeLeafEdges(tableP, entryP, nodeP->rightP, fieldP);
        break;

    case ADDR:
    case LCLADDR:
    case BREF:
    case WILDREF:
    case CONSTANT:
        if( nodeP != fieldP )
        {
            makeTakenEdge(tableP, entryP, nodeP);
        }
        break;

    default:
        break;
    }
}

// Make the edges of an instruction's address field, to the decoded address
// in the word's own bank; implicit unless the field was a single symbol.
// jda writes its word and enters the next; cal writes 100, enters 101 and
// ignores its field, so a symbol in a cal word is only taken.
static void
makeInstructionEdges(OptTableP tableP, OptWordP entryP, PNodeP fieldP)
{
OptDecodeP decodeP;
const MemrefRole *roleP;
unsigned int flags;
SymNodeP symP;
int addr;

    decodeP = &entryP->decode;
    addr = decodeP->address;
    symP = (fieldP)?leafSymbol(fieldP):NILP;
    flags = (fieldP)?0:OPTEF_IMPLICIT;

    if( decodeP->opcode == 016 )
    {
        if( decodeP->indirect )
        {
            // jda: deposit AC in the word, continue at the next.
            addEdge(tableP, entryP, entryP->bank, addr, OPTR_WRITE, flags, symP);
            addEdge(tableP, entryP, entryP->bank, ((addr + 1) & ADDRMASK), OPTR_JUMP, OPTEF_IMPLICIT, NILP);
        }
        else
        {
            // cal: jda 100.
            addEdge(tableP, entryP, entryP->bank, CAL_WRITE_ADDR, OPTR_WRITE, OPTEF_IMPLICIT, NILP);
            addEdge(tableP, entryP, entryP->bank, CAL_JUMP_ADDR, OPTR_JUMP, OPTEF_IMPLICIT, NILP);

            if( fieldP )
            {
                makeTakenEdge(tableP, entryP, fieldP);
            }
        }

        return;
    }

    roleP = &memrefRoles[decodeP->opcode >> 1];

    if( roleP->role == OPTR_COUNT )
    {
        return;
    }

    flags |= roleP->flags;

    if( decodeP->memIndirect )
    {
        flags |= OPTEF_INDIRECT;
    }

    addEdge(tableP, entryP, entryP->bank, addr, roleP->role, flags, symP);

    if( roleP->secondRole != OPTR_COUNT )
    {
        addEdge(tableP, entryP, entryP->bank, addr, roleP->secondRole, flags, symP);
    }
}

// Make a taken edge from a word to the word a symbol leaf names.
static void
makeTakenEdge(OptTableP tableP, OptWordP entryP, PNodeP leafP)
{
int bank;
int addr;

    if( leafTarget(tableP, entryP, leafP, &bank, &addr) )
    {
        addEdge(tableP, entryP, bank, addr, OPTR_TAKEN, 0, leafSymbol(leafP));
    }
}

// Where does a symbol leaf point?  A BREF names a word in its qualifying
// bank; anything else (a pool CONSTANT included) the referencing word's bank.
// Returns 1 with the bank and address stored, 0 (reported) if unresolved.
static int
leafTarget(OptTableP tableP, OptWordP entryP, PNodeP leafP, int *bankP, int *addrP)
{
SymNodeP symP;

    symP = leafSymbol(leafP);

    if( !symP || ((leafP->type == CONSTANT) && !(symP->flags & SYMF_ASSIGNED)) ||
        ((leafP->type != CONSTANT) && !(symP->flags & SYMF_RESOLVED)) )
    {
        fprintf(stderr, "am1: optimizer: line %d: reference to %s never resolved, skipped\n",
            entryP->lineNo, (symP)?symP->name:((leafP->type == WILDREF)?leafP->value.strP:"?"));
        ++tableP->unresolvedCount;
        return(0);
    }

    *bankP = (leafP->type == BREF)?leafP->value2.ival:entryP->bank;
    *addrP = (symP->value & ADDRMASK);

    if( (*bankP < 0) || (*bankP > MAXBANK) )
    {
        fprintf(stderr, "am1: optimizer: line %d: reference to %s names bank %d, skipped\n",
            entryP->lineNo, symP->name, *bankP);
        ++tableP->unresolvedCount;
        return(0);
    }

    return(1);
}

// The symbol a leaf carries.
// Returns the SymNode, or NILP for a WILDREF (which carries only a name).
static SymNodeP
leafSymbol(PNodeP leafP)
{
    if( !leafP || (leafP->type == WILDREF) )
    {
        return(NILP);
    }

    return( leafP->value.symP );
}

// Add an edge to every word at a bank and address: one per overlaid entry,
// so each reverse index is complete, or one OPTEF_NOWORD edge if none.
// Returns the first edge made.
static OptEdgeP
addEdge(OptTableP tableP, OptWordP fromP, int bank, int addr, OptRole role, unsigned int flags,
    SymNodeP symP)
{
OptBankP bankP;
OptWordP toP;
OptEdgeP firstP;
OptEdgeP edgeP;

    bankP = ((bank >= 0) && (bank <= MAXBANK))?tableP->banksP[bank]:NILP;
    toP = ((bankP && (addr >= 0) && (addr < BANKSIZE)))?bankP->wordsP[addr]:NILP;

    if( !toP )
    {
        ++tableP->noWordCount;
        return( newEdge(tableP, fromP, NILP, bank, addr, role, (flags | OPTEF_NOWORD), symP) );
    }

    firstP = NILP;

    for( ; toP; toP = toP->sameAddrP )
    {
        edgeP = newEdge(tableP, fromP, toP, bank, addr, role, flags, symP);

        if( !firstP )
        {
            firstP = edgeP;
        }
    }

    return(firstP);
}

// Make one edge and link it into the out and in lists and the statistics.
// Returns the edge; allocation failure is fatal.
static OptEdgeP
newEdge(OptTableP tableP, OptWordP fromP, OptWordP toP, int bank, int addr, OptRole role,
    unsigned int flags, SymNodeP symP)
{
OptEdgeP edgeP;

    if( !(edgeP = (OptEdgeP)calloc(1, sizeof(OptEdge))) )
    {
        fprintf(stderr, "am1: out of memory building the optimizer reference edges\n");
        exit(1);
    }

    edgeP->fromP = fromP;
    edgeP->toP = toP;
    edgeP->toBank = bank;
    edgeP->toAddr = addr;
    edgeP->role = role;
    edgeP->flags = flags;
    edgeP->symP = symP;

    if( fromP->outTailP )
    {
        fromP->outTailP->nextOutP = edgeP;
    }
    else
    {
        fromP->outP = edgeP;
    }

    fromP->outTailP = edgeP;
    ++fromP->outCount;

    if( toP )
    {
        if( toP->inTailP )
        {
            toP->inTailP->nextInP = edgeP;
        }
        else
        {
            toP->inP = edgeP;
        }

        toP->inTailP = edgeP;

        // deriveFlags() computes the in counts from this list.
    }

    ++tableP->edgeCount;
    ++tableP->roleCounts[role];

    if( flags & OPTEF_INDIRECT )
    {
        ++tableP->indirectCount;
    }

    if( flags & OPTEF_VIAPOINTER )
    {
        ++tableP->viaCount;
    }

    return(edgeP);
}

// Free one word's out list.  The in lists share these edges.
void
freeEdgeList(OptEdgeP edgeP)
{
OptEdgeP nextP;

    while( edgeP )
    {
        nextP = edgeP->nextOutP;
        free(edgeP);
        edgeP = nextP;
    }
}

// Passes 2 and 4: derive every word's in counts and edge-implied flags from
// scratch; the "maybe" marks and the start flag are left alone.  An indirect
// edge counts as a read, since the hardware only reads the pointer word; its
// role reaches the real target by the via-pointer edge or the maybe marks.
static void
deriveFlags(OptTableP tableP)
{
int i;
int r;
OptWordP entryP;
OptEdgeP edgeP;

    for( i = 0; i < tableP->count; ++i )
    {
        entryP = tableP->entriesPP[i];
        entryP->flags &= ~(OPTF_WRITTEN | OPTF_PATCHED | OPTF_TAKEN | OPTF_XCTTARGET | OPTF_READ | OPTF_JUMPTARGET);

        for( r = 0; r < OPTR_COUNT; ++r )
        {
            entryP->inCounts[r] = 0;
        }

        for( edgeP = entryP->inP; edgeP; edgeP = edgeP->nextInP )
        {
            if( edgeP->flags & OPTEF_PLACEHOLDER )
            {
                continue;
            }

            ++entryP->inCounts[(edgeP->flags & OPTEF_INDIRECT)?OPTR_READ:edgeP->role];
        }

        if( entryP->inCounts[OPTR_WRITE] )
        {
            entryP->flags |= OPTF_WRITTEN;
        }

        if( entryP->inCounts[OPTR_TAKEN] )
        {
            entryP->flags |= OPTF_TAKEN;
        }

        if( entryP->inCounts[OPTR_EXECUTE] )
        {
            entryP->flags |= OPTF_XCTTARGET;
        }

        if( entryP->inCounts[OPTR_READ] )
        {
            entryP->flags |= OPTF_READ;
        }

        if( entryP->inCounts[OPTR_JUMP] )
        {
            entryP->flags |= OPTF_JUMPTARGET;
        }

        for( edgeP = entryP->inP; edgeP; edgeP = edgeP->nextInP )
        {
            if( (edgeP->role == OPTR_WRITE) && (edgeP->flags & OPTEF_PATCH) &&
                !(edgeP->flags & (OPTEF_INDIRECT | OPTEF_PLACEHOLDER)) )
            {
                entryP->flags |= OPTF_PATCHED;
                break;
            }
        }
    }
}

// Pass 2, second half: a patched word's implicit edges ("rtn, jmp 0") point
// at nothing real; a symbol-named field is real at least once and is kept.
// A cascade through a patched "dap 0" is not iterated.
static void
markPlaceholders(OptTableP tableP)
{
int i;
OptWordP entryP;
OptEdgeP edgeP;

    for( i = 0; i < tableP->count; ++i )
    {
        entryP = tableP->entriesPP[i];

        if( !(entryP->flags & OPTF_PATCHED) )
        {
            continue;
        }

        for( edgeP = entryP->outP; edgeP; edgeP = edgeP->nextOutP )
        {
            if( (edgeP->flags & OPTEF_IMPLICIT) && !(edgeP->flags & OPTEF_PLACEHOLDER) )
            {
                edgeP->flags |= OPTEF_PLACEHOLDER;
                ++tableP->placeholderCount;
            }
        }
    }
}

// Pass 3: follow each indirect edge's pointer word, adding a via-pointer
// edge in the same role or marking the bank conservatively.  The via edges
// are appended to the lists being walked and skipped when reached.
static void
followPointers(OptTableP tableP)
{
int i;
int bank;
int addr;
unsigned int flags;
OptWordP entryP;
OptEdgeP edgeP;

    for( i = 0; i < tableP->count; ++i )
    {
        entryP = tableP->entriesPP[i];

        for( edgeP = entryP->outP; edgeP; edgeP = edgeP->nextOutP )
        {
            if( !(edgeP->flags & OPTEF_INDIRECT) || (edgeP->flags & (OPTEF_VIAPOINTER | OPTEF_PLACEHOLDER)) )
            {
                continue;
            }

            if( !(edgeP->flags & OPTEF_NOWORD) &&
                followPointer(tableP, edgeP->toP, entryP, &bank, &addr) )
            {
                flags = ((edgeP->flags & OPTEF_PATCH) | OPTEF_VIAPOINTER);

                if( bank != entryP->bank )
                {
                    flags |= OPTEF_CROSSBANK;
                }

                addEdge(tableP, entryP, bank, addr, edgeP->role, flags, NILP);
            }
            else
            {
                edgeP->flags |= OPTEF_UNKNOWN;
                ++tableP->unknownIndirectCount;
                markConservatively(tableP, entryP->bank, edgeP->role);
            }
        }
    }
}

// Can a pointer word be followed, and where to?  Only if it is data, not
// overlaid, never directly written (a write through another pointer is not
// seen) and its value has bits 0 and 1 clear.  A bank-qualified source
// ("[foo:0]") is a 16-bit pointer into that bank, the memory.ah convention
// for cross-bank calls under eem.  Otherwise no bank bits means the
// referencing bank, as the hardware does with extend mode off; bank bits
// into a bank that emitted nothing are more likely a chained-indirect word.
// Returns 1 with the target stored, 0 if the pointer cannot be followed.
static int
followPointer(OptTableP tableP, OptWordP pointerP, OptWordP fromP, int *bankP, int *addrP)
{
int bank;
int exprBank;

    if( pointerP->flags & (OPTF_RESERVED | OPTF_DUPADDR) )
    {
        return(0);
    }

    if( hasDirectWrite(pointerP) || pointerP->inCounts[OPTR_JUMP] || pointerP->inCounts[OPTR_EXECUTE] )
    {
        return(0);
    }

    if( (pointerP->kind == OPTK_EXPR) && (pointerP->decode.spelling != OPTS_DATA) &&
        ((pointerP->decode.spelling != OPTS_CONSTREF) || pointerP->decode.opSymP) )
    {
        return(0);
    }

    if( pointerP->value & ~0177777 )
    {
        return(0);
    }

    bank = ((pointerP->value >> 12) & 017);
    exprBank = pointerExprBank(pointerP->exprP);

    if( exprBank >= 0 )
    {
        bank = exprBank;
    }
    else if( !bank )
    {
        bank = fromP->bank;
    }
    else if( !tableP->banksP[bank] )
    {
        return(0);
    }

    *bankP = bank;
    *addrP = (pointerP->value & ADDRMASK);
    return(1);
}

// Which bank does a pointer word's expression name?  The first BREF leaf
// decides, looking through arithmetic, so "[foo:0+1]" names bank 0.
// Returns the bank, or -1 when the expression has no bank qualifier.
static int
pointerExprBank(PNodeP nodeP)
{
int bank;

    if( !nodeP )
    {
        return(-1);
    }

    switch( nodeP->type )
    {
    case BINOP:
        if( (bank = pointerExprBank(nodeP->leftP)) >= 0 )
        {
            return(bank);
        }

        return( pointerExprBank(nodeP->rightP) );

    case UNOP:
        return( pointerExprBank(nodeP->rightP) );

    case BREF:
        return( nodeP->value2.ival );

    default:
        return(-1);
    }
}

// Does a direct write edge enter a word?  Via-pointer and indirect writes
// do not count: an indirect write only reads the pointer.
// Returns 1 if so, 0 if not.
static int
hasDirectWrite(OptWordP entryP)
{
OptEdgeP edgeP;

    for( edgeP = entryP->inP; edgeP; edgeP = edgeP->nextInP )
    {
        if( (edgeP->role == OPTR_WRITE) &&
            !(edgeP->flags & (OPTEF_VIAPOINTER | OPTEF_INDIRECT | OPTEF_PLACEHOLDER)) )
        {
            return(1);
        }
    }

    return(0);
}

// Does a write edge other than a patch enter a word?  A dap or dip argues
// for an instruction, so classifyWords()' data rule asks this instead.
// Returns 1 if so, 0 if not.
static int
hasPlainWrite(OptWordP entryP)
{
OptEdgeP edgeP;

    for( edgeP = entryP->inP; edgeP; edgeP = edgeP->nextInP )
    {
        if( (edgeP->role == OPTR_WRITE) &&
            !(edgeP->flags & (OPTEF_PATCH | OPTEF_INDIRECT | OPTEF_PLACEHOLDER)) )
        {
            return(1);
        }
    }

    return(0);
}

// An unfollowed pointer may reach any taken word: mark every taken word of
// the bank as possibly reached in the role, and record the bank's marks.
static void
markConservatively(OptTableP tableP, int bank, OptRole role)
{
int i;
unsigned int mark;
OptWordP entryP;

    switch( role )
    {
    case OPTR_READ:
        mark = OPTF_MAYBE_READ;
        break;

    case OPTR_WRITE:
        mark = OPTF_MAYBE_WRITTEN;
        break;

    case OPTR_JUMP:
    case OPTR_EXECUTE:
        mark = OPTF_MAYBE_ENTERED;
        break;

    default:
        return;
    }

    tableP->conservativeBanks[bank] |= mark;

    for( i = 0; i < tableP->count; ++i )
    {
        entryP = tableP->entriesPP[i];

        if( (entryP->bank == bank) && (entryP->flags & OPTF_TAKEN) )
        {
            entryP->flags |= mark;
        }
    }
}

// Pass 5: the start flag, the code/data classification and the conflicts:
//
//   code: a jump or execute edge comes in, or the word is at the start
//         address, or it is an expression spelled as an instruction (a
//         mnemonic with or without an operand, a law, a mnemonic with a
//         [..] operand, or one lone mnemonic inside arithmetic, as in
//         "dpy-i LIGHTPEN"); optflow.c refines the spelling case by
//         reachability.
//   data: the word's kind is text, ascii, type340, table, var or const; or
//         read edges, or write edges other than dap and dip, come in and no
//         jump or execute edge does; or it is an expression spelled as plain
//         data (no mnemonic: a number, a symbol, arithmetic, a bare [..])
//         and no jump or execute edge comes in.  A dap or dip alone argues
//         for code, not data: it is how an instruction's address field is
//         varied, and the patched flag already records it.
//
// The two are not exclusive; where both hold the word is a conflict, as is
// code that is written at all and code that is patched.
static void
classifyWords(OptTableP tableP)
{
int i;
int transferIn;
int asInstruction;
int asData;
OptWordP entryP;
OptBankP bankP;

    if( tableP->hasStart )
    {
        i = ((tableP->startAddr >> 12) & 017);

        if( (bankP = tableP->banksP[i]) )
        {
            for( entryP = bankP->wordsP[tableP->startAddr & ADDRMASK]; entryP; entryP = entryP->sameAddrP )
            {
                entryP->flags |= OPTF_START;
            }
        }
    }

    for( i = 0; i < tableP->count; ++i )
    {
        entryP = tableP->entriesPP[i];
        entryP->flags &= ~(OPTF_CODE | OPTF_DATA);
        entryP->conflicts = 0;

        transferIn = ((entryP->flags & (OPTF_JUMPTARGET | OPTF_XCTTARGET)) != 0);
        asInstruction = 0;
        asData = 0;

        if( (entryP->kind == OPTK_EXPR) && !(entryP->flags & OPTF_RESERVED) )
        {
            switch( entryP->decode.spelling )
            {
            case OPTS_MNEMONIC_OPERAND:
            case OPTS_MNEMONIC_ONLY:
            case OPTS_LAW:
                asInstruction = 1;
                break;

            case OPTS_CONSTREF:
                if( entryP->decode.opSymP )
                {
                    asInstruction = 1;
                }
                else
                {
                    asData = 1;
                }
                break;

            case OPTS_DATA:
                asData = 1;
                break;

            case OPTS_OTHER:
                // One opcode symbol inside arithmetic still spells an
                // instruction, as in the lightpen idiom "dpy-i LIGHTPEN".
                // Calling it neither would make a plain IOT not code and stop
                // the flow walk there.  Two mnemonics ("jmp x cla") leave the
                // instruction in doubt and are not called code.
                if( entryP->decode.opSymCount == 1 )
                {
                    asInstruction = 1;
                }
                break;

            default:
                // A shape that argues for neither.
                break;
            }
        }

        if( transferIn || (entryP->flags & OPTF_START) || asInstruction )
        {
            entryP->flags |= OPTF_CODE;
        }

        if( (entryP->kind != OPTK_EXPR) ||
            (!transferIn && ((entryP->flags & OPTF_READ) || hasPlainWrite(entryP) || asData)) )
        {
            entryP->flags |= OPTF_DATA;
        }

        if( entryP->flags & OPTF_CODE )
        {
            if( entryP->flags & OPTF_DATA )
            {
                entryP->conflicts |= OPTCF_CODE_DATA;
            }

            if( entryP->flags & OPTF_PATCHED )
            {
                entryP->conflicts |= OPTCF_CODE_PATCHED;
            }

            if( entryP->flags & OPTF_WRITTEN )
            {
                entryP->conflicts |= OPTCF_CODE_WRITTEN;
            }
        }
    }
}

// Print one edge for the dump: the role, the flags, the far end, and for an
// out edge the symbol the source wrote.
static void
printEdge(FILE *fP, OptEdgeP edgeP, int outgoing)
{
    fprintf(fP, "%s", optRoleName(edgeP->role));

    if( edgeP->flags & OPTEF_INDIRECT )
    {
        fprintf(fP, " i");
    }

    if( edgeP->flags & OPTEF_VIAPOINTER )
    {
        fprintf(fP, " via");
    }

    if( edgeP->flags & OPTEF_PATCH )
    {
        fprintf(fP, " patch");
    }

    if( edgeP->flags & OPTEF_IMPLICIT )
    {
        fprintf(fP, " implicit");
    }

    if( edgeP->flags & OPTEF_CROSSBANK )
    {
        fprintf(fP, " crossbank");
    }

    if( edgeP->flags & OPTEF_UNKNOWN )
    {
        fprintf(fP, " unknown");
    }

    if( edgeP->flags & OPTEF_PLACEHOLDER )
    {
        fprintf(fP, " placeholder");
    }

    if( outgoing )
    {
        fprintf(fP, "->%02o%04o%s", edgeP->toBank, edgeP->toAddr, (edgeP->flags & OPTEF_NOWORD)?"?":"");

        if( edgeP->symP )
        {
            // A pool symbol's name is its fingerprint; show [..].
            if( edgeP->symP->flags & (SYMF_ASSIGNED | SYMF_EVALED | SYMF_EMITTED) )
            {
                fprintf(fP, " [..]");
            }
            else
            {
                fprintf(fP, " %s", edgeP->symP->name);
            }
        }
    }
    else
    {
        fprintf(fP, "<-%02o%04o", edgeP->fromP->bank, edgeP->fromP->addr);
    }
}

// The report's reference and classification lines for one bank.
void
writeReferenceReport(FILE *fP, OptTableP tableP, int bank)
{
int i;
int r;
int codeCount;
int dataCount;
int bothCount;
int neitherCount;
int writtenCount;
int patchedCount;
int takenCount;
int xctCount;
int edgeCount;
int indirectCount;
int viaCount;
int unknownCount;
int placeholderCount;
int roleCounts[OPTR_COUNT];
int conflictCounts[3];
unsigned int marks;
OptWordP entryP;
OptEdgeP edgeP;

    codeCount = dataCount = bothCount = neitherCount = 0;
    writtenCount = patchedCount = takenCount = xctCount = 0;
    edgeCount = indirectCount = viaCount = unknownCount = placeholderCount = 0;

    for( r = 0; r < OPTR_COUNT; ++r )
    {
        roleCounts[r] = 0;
    }

    for( r = 0; r < 3; ++r )
    {
        conflictCounts[r] = 0;
    }

    for( i = 0; i < tableP->count; ++i )
    {
        entryP = tableP->entriesPP[i];

        if( entryP->bank != bank )
        {
            continue;
        }

        if( (entryP->flags & (OPTF_CODE | OPTF_DATA)) == (OPTF_CODE | OPTF_DATA) )
        {
            ++bothCount;
        }
        else if( entryP->flags & OPTF_CODE )
        {
            ++codeCount;
        }
        else if( entryP->flags & OPTF_DATA )
        {
            ++dataCount;
        }
        else
        {
            ++neitherCount;
        }

        if( entryP->flags & OPTF_WRITTEN )
        {
            ++writtenCount;
        }

        if( entryP->flags & OPTF_PATCHED )
        {
            ++patchedCount;
        }

        if( entryP->flags & OPTF_TAKEN )
        {
            ++takenCount;
        }

        if( entryP->flags & OPTF_XCTTARGET )
        {
            ++xctCount;
        }

        for( r = 0; r < 3; ++r )
        {
            if( entryP->conflicts & conflictNames[r].bit )
            {
                ++conflictCounts[r];
            }
        }

        for( edgeP = entryP->outP; edgeP; edgeP = edgeP->nextOutP )
        {
            ++edgeCount;
            ++roleCounts[edgeP->role];

            if( edgeP->flags & OPTEF_INDIRECT )
            {
                ++indirectCount;
            }

            if( edgeP->flags & OPTEF_VIAPOINTER )
            {
                ++viaCount;
            }

            if( edgeP->flags & OPTEF_UNKNOWN )
            {
                ++unknownCount;
            }

            if( edgeP->flags & OPTEF_PLACEHOLDER )
            {
                ++placeholderCount;
            }
        }
    }

    fprintf(fP, "    classified: code %d, data %d, code and data %d, neither %d; written %d (patched %d), taken %d, xct targets %d\n",
        codeCount, dataCount, bothCount, neitherCount, writtenCount, patchedCount, takenCount, xctCount);

    fprintf(fP, "    references: %d edges:", edgeCount);

    for( r = 0; r < OPTR_COUNT; ++r )
    {
        fprintf(fP, " %s %d%s", optRoleName((OptRole)r), roleCounts[r], (r < (OPTR_COUNT - 1))?",":";");
    }

    fprintf(fP, " %d indirect, %d followed through a pointer, %d not followed; %d placeholders in patched words\n",
        indirectCount, viaCount, unknownCount, placeholderCount);

    if( (marks = tableP->conservativeBanks[bank]) )
    {
        fprintf(fP, "    conservative: every taken word in this bank is possibly");

        if( marks & OPTF_MAYBE_READ )
        {
            fprintf(fP, " read");
        }

        if( marks & OPTF_MAYBE_WRITTEN )
        {
            fprintf(fP, "%s written", (marks & OPTF_MAYBE_READ)?",":"");
        }

        if( marks & OPTF_MAYBE_ENTERED )
        {
            fprintf(fP, "%s entered", (marks & (OPTF_MAYBE_READ | OPTF_MAYBE_WRITTEN))?",":"");
        }

        fprintf(fP, " through a pointer that could not be followed\n");
    }

    fprintf(fP, "    conflicts: code and data %d, code and patched %d, code and written %d\n",
        conflictCounts[0], conflictCounts[1], conflictCounts[2]);
}

// The report's list of every word in conflict: bank, address, source line,
// the first label at the word, and which conflicts it has.
void
writeConflictList(FILE *fP, OptTableP tableP)
{
int i;
int c;
int total;
int any;
OptWordP entryP;

    total = 0;

    for( i = 0; i < tableP->count; ++i )
    {
        if( tableP->entriesPP[i]->conflicts )
        {
            ++total;
        }
    }

    if( !total )
    {
        fprintf(fP, "\nClassification conflicts: none\n");
        return;
    }

    fprintf(fP, "\nClassification conflicts: %d words\n", total);

    for( i = 0; i < tableP->count; ++i )
    {
        entryP = tableP->entriesPP[i];

        if( !entryP->conflicts )
        {
            continue;
        }

        fprintf(fP, "  bank %2d %04o line %5d  %-16s", entryP->bank, entryP->addr, entryP->lineNo,
            firstLabelName(entryP));

        any = 0;

        for( c = 0; c < 3; ++c )
        {
            if( entryP->conflicts & conflictNames[c].bit )
            {
                fprintf(fP, "%s%s", (any)?", ":" ", conflictNames[c].nameP);
                any = 1;
            }
        }

        fprintf(fP, "\n");
    }
}

// The first label attached to a word, for the report.
// Returns the label's name, or "-" when the word has none.
const char *
firstLabelName(OptWordP entryP)
{
    if( entryP->labelsP && entryP->labelsP->symP && entryP->labelsP->symP->name )
    {
        return( entryP->labelsP->symP->name );
    }

    return("-");
}
