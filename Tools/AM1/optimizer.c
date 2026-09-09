/* optimizer.c - the am1 optimizer: word table builder, report and debug dump
 *
 * Purpose:
 *   Phase 1 of the am1 optimizer is an analysis-only advisor (see
 *   Optimizer/FeasibilityStudy.md, sections 5 and 7).  This file holds the
 *   foundation every analysis stands on: the word table, one entry per word
 *   the program emits, in emission order, with its bank, address, value, the
 *   node or symbol that produced it, its kind, the labels defined at it and
 *   its source line.  The instruction decoder (task A2), the reference edges
 *   (A3), the control-flow overlay (A4) and the rule engine and report (A5)
 *   are added to this file and to optimizer.h by later tasks.
 *
 * Architectural scope:
 *   Runs once, after yyparse() has returned and before any code generator,
 *   called from main() in am1.c when -O was given.  It reads the parse tree
 *   and the resolved symbol tables and never modifies either; everything it
 *   derives lives in the OptTable declared in optimizer.h.  The table builder
 *   walks the statement list exactly the way the code generators do, so that
 *   the table contains the same words, in the same order, at the same
 *   addresses, as the tape.  That walk is deliberately a copy of the one in
 *   bincodegen.c and testcodegen.c rather than a call into them: the
 *   generators are static to their files, they write output as they go, and
 *   the phase 1 ground rules say they are not to be touched.
 *
 * Dependencies:
 *   am1.h and y.tab.h for the tree, the symbol table and the node type
 *   tokens; evalExpr() from eval.c; the bank context list banksP from
 *   parser.y; the report FILE outfP that am1.c opens on <basename>.opt.
 *
 * Execution model:
 *   Single threaded.  optimize() builds the table, optionally prints the
 *   debug dump, writes the report and frees the table.  Memory is allocated
 *   with malloc/calloc and released by optFreeTable(); a failure to allocate
 *   is fatal, as it is everywhere else in am1.
 *
 * Verification:
 *   The debug dump (-O=dump) prints the table in the -T test dump's format so
 *   the two can be diffed directly; wordtable_check.sh in this directory does
 *   that for every regression test and also checks the table against the
 *   words decoded from the tape image.
 *
 * Revision history:
 *
 * 08-Sep-2026 claude - initial version, task A1: -O flag, word table, debug dump
 * 08-Sep-2026 claude - task A2: instruction decoder, spelling classification,
 *                      operate-group phase table, -O=decode and -O=check
 * 08-Sep-2026 claude - task A3: reference edges with roles, the reverse index,
 *                      pointer following, the written/patched/taken/code/data
 *                      flags, conflicts, -O=refs
 * 08-Sep-2026 claude - task A4: basic blocks, control-flow edges, the UNKNOWN
 *                      sink, reachability, the label/after-skip flags, -O=flow
 * 08-Sep-2026 claude - task A5: the preconditions, the twelve rules, the
 *                      findings, the instruction timing, the report the whole
 *                      advisor exists to write, and -O=rules
 * 08-Sep-2026 claude - task A7: -O documented in Docs-OPTIMIZER.md; the unused tableP
 *                      parameter of tempIsPrivate() marked, the only -Wall -Wextra warning
 * 08-Sep-2026 claude - task A6a: instruction code 12 is unused.  It decoded as
 *                      the memory reference jfd because permsyms.def defined
 *                      one there; permsyms.def no longer does and the owner
 *                      has ruled the code spare, so it decodes as unknown and
 *                      the jump role, the terminator, the sink and the
 *                      report's caveat clause are all gone
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

// The running emission state the builder keeps while it walks the tree.  It
// mirrors cur_bank/cur_pc in bincodegen.c so the address the binary generator
// WOULD write a word at can be checked against the address the parser stamped
// on the node.  The two have disagreed before (rg33 in the regression suite
// pins one such case), and a table built from the wrong one is worthless.
typedef struct
{
    int bank;               // the bank the binary generator would be writing
    int pc;                 // and the address within it
    const char *fileP;      // the source file the words are coming from, from the
                            // last FILENAME statement the walk passed (task A5)
    bool failed;            // set when the walk met something it could not place
} Builder, *BuilderP;

#define INITIAL_CAPACITY    256     // entries allocated for a new table, doubled as needed

extern BankContextP banksP;         // every bank the program used, most recent first
extern FILE *outfP;                 // the report file, opened by am1.c
extern char *origFilenameP;         // the source file named on the command line

extern int evalExpr(PNodeP);

// Set by -O=dump: print the word table on stdout in -T format.
static bool optDumpWanted;

// Set by -O=decode: print the decoded dump on stdout (task A2).
static bool optDecodeDumpWanted;

// Set by -O=check: run the decoder's self-check first and print it on stdout.
static bool optCheckWanted;

// Set by -O=refs: print the reference dump on stdout (task A3).
static bool optRefsDumpWanted;

// Set by -O=flow: print the control-flow dump on stdout (task A4).
static bool optFlowDumpWanted;

// Set by -O=rules: print the findings on stdout (task A5).
static bool optRulesDumpWanted;

// The builder's running state for the current optimize() call.
static Builder builder;

static OptTableP newTable(void);
static OptWordP addEntry(OptTableP tableP, int bank, int addr, int value, OptKind kind,
    PNodeP nodeP, PNodeP exprP, SymNodeP symP, int lineNo, unsigned int flags);
static void indexEntry(OptTableP tableP, OptWordP entryP);
static void addLabelDef(OptTableP tableP, SymNodeP symP, int bank, int addr);
static void attachLabels(OptTableP tableP);
static void freeLabelList(OptLabelP labelP);

static void walkStatements(OptTableP tableP, PNodeP nodeP);
static void addExprWord(OptTableP tableP, PNodeP nodeP, PNodeP exprP);
static void addTextWords(OptTableP tableP, PNodeP nodeP, PNodeP textNodeP, OptKind kind);
static void addAsciiWords(OptTableP tableP, PNodeP nodeP, PNodeP asciiNodeP);
static void addTableWords(OptTableP tableP, PNodeP nodeP);
static void addVarWords(OptTableP tableP, PNodeListP listP, int bank);
static void addConstWords(OptTableP tableP, SymNodeP symP, int bank);

static int canReduce(PNodeP nodeP);
static int reduceOperand(PNodeP nodeP);
static void advancePC(int incr);
static void checkPlacement(OptTableP tableP, OptWordP entryP);

static void writeReport(FILE *fP, OptTableP tableP, char *basenameP);
static const char *kindName(OptKind kind);

// Task A3 (the definitions are at the end of the file).
static void freeEdgeList(OptEdgeP edgeP);
static void writeReferenceReport(FILE *fP, OptTableP tableP, int bank);
static void writeConflictList(FILE *fP, OptTableP tableP);

// Task A4 (the definitions are at the end of the file).
static void freeBlockList(OptTableP tableP);
static void writeFlowReport(FILE *fP, OptTableP tableP, int bank);
static void writeUnreachedList(FILE *fP, OptTableP tableP);

// Task A5 (the definitions are at the end of the file).
static void freeFindingList(OptFindingP findingP);
static void writeHeaderNotes(FILE *fP);
static void writeFindingList(FILE *fP, OptTableP tableP);
static void writeSuppressedList(FILE *fP, OptTableP tableP);
static void writeStatistics(FILE *fP, OptTableP tableP);

// Accept one modifier given as -O=modifier on the command line.
// "dump" turns on the debug dump of the word table; "decode" the decoded
// dump; "refs" the reference dump; "flow" the control-flow dump; "rules" the
// one-line-per-finding dump; "check" the decoder's self-check.
// Returns 1 if the modifier was recognized and applied, 0 if it was not,
// in which case the caller treats the command line as invalid.
int
optimizeSetOption(char *nameP)
{
    if( !strcmp(nameP, "dump") )
    {
        optDumpWanted = true;
        return(1);
    }

    if( !strcmp(nameP, "decode") )
    {
        optDecodeDumpWanted = true;
        return(1);
    }

    if( !strcmp(nameP, "refs") )
    {
        optRefsDumpWanted = true;
        return(1);
    }

    if( !strcmp(nameP, "flow") )
    {
        optFlowDumpWanted = true;
        return(1);
    }

    if( !strcmp(nameP, "rules") )
    {
        optRulesDumpWanted = true;
        return(1);
    }

    if( !strcmp(nameP, "check") )
    {
        optCheckWanted = true;
        return(1);
    }

    return(0);
}

// Run the optimizer: self-check the decoder if asked, build the word table,
// decode every word, build the reference edges and classify the words, dump
// any view that was asked for, write the report to outfP, release the table.
// Returns 1 on success, 0 if the self-check failed or the table could not
// be built; the caller removes the report file on 0.
int
optimize(PNodeP rootP, char *basenameP)
{
OptTableP tableP;

    if( optCheckWanted )
    {
        if( !optDecoderSelfCheck(stdout) )
        {
            fflush(stdout);
            return(0);
        }

        fflush(stdout);
    }

    if( !(tableP = optBuildTable(rootP)) )
    {
        return(0);
    }

    optDecodeTable(tableP);
    optBuildReferences(tableP);
    optBuildFlow(tableP);
    optRunRules(tableP);

    if( optDumpWanted )
    {
        optDumpTable(stdout, tableP);
        fflush(stdout);
    }

    if( optDecodeDumpWanted )
    {
        optDumpDecoded(stdout, tableP);
        fflush(stdout);
    }

    if( optRefsDumpWanted )
    {
        optDumpReferences(stdout, tableP);
        fflush(stdout);
    }

    if( optFlowDumpWanted )
    {
        optDumpFlow(stdout, tableP);
        fflush(stdout);
    }

    if( optRulesDumpWanted )
    {
        optDumpRules(stdout, tableP);
        fflush(stdout);
    }

    writeReport(outfP, tableP, basenameP);
    optFreeTable(tableP);
    return(1);
}

// Build the word table for a parsed program whose root (a HEADER node) is
// rootP.  The walk is the code generators' walk: the statement list hangs
// off rootP->leftP, the trailing 'start' or 'stop' off rootP->rightP, and
// the constants and variables never placed by an explicit directive are
// taken from the bank contexts after the list has been walked.
// Returns the new table, or NILP if the tree held something that could not
// be placed; the reason has already been printed on stderr.
OptTableP
optBuildTable(PNodeP rootP)
{
OptTableP tableP;
BankContextP bankP;

    tableP = newTable();

    // The binary generator starts where macro1 did, bank 0 address 4.
    builder.bank = 0;
    builder.pc = 4;
    builder.failed = false;

    // The source named on the command line, until a FILENAME statement says
    // otherwise.  cpp's first line marker is consumed by the grammar's
    // optfilenames rule and never becomes a statement, so a program with no
    // includes has no FILENAME node at all and this is the only name its
    // words ever get.
    builder.fileP = origFilenameP;

    if( rootP->rightP && (rootP->rightP->type == START) )
    {
        tableP->hasStart = 1;
        tableP->startAddr = rootP->rightP->value.ival;
    }

    walkStatements(tableP, rootP->leftP);

    // Now the constants and variables that had no explicit 'constants' or
    // 'variables' directive.  The parser placed them at the end of their own
    // bank, constants first, then variables, and recorded where each block
    // starts; every generator emits them in that order.
    for( bankP = banksP; bankP && !builder.failed; bankP = bankP->nextP )
    {
        if( !bankP->constSymP && !bankP->varNodesP )
        {
            continue;
        }

        builder.bank = bankP->bank;
        builder.pc = (bankP->constSymP)?bankP->constPC:bankP->varPC;

        if( bankP->constSymP )
        {
            addConstWords(tableP, bankP->constSymP, bankP->bank);
        }

        if( bankP->varNodesP )
        {
            addVarWords(tableP, bankP->varNodesP, bankP->bank);
        }
    }

    if( builder.failed )
    {
        optFreeTable(tableP);
        return(NILP);
    }

    attachLabels(tableP);
    return(tableP);
}

// Print the table in the -T test dump's format: one line per emitted word,
// the bank and address as one six digit octal number and the value as
// another.  Reserved table words print nothing, as in -T.
void
optDumpTable(FILE *fP, OptTableP tableP)
{
int i;
OptWordP entryP;

    for( i = 0; i < tableP->count; ++i )
    {
        entryP = tableP->entriesPP[i];

        if( entryP->flags & OPTF_RESERVED )
        {
            continue;
        }

        fprintf(fP, "%02o%04o %06o\n", entryP->bank, entryP->addr, entryP->value);
    }
}

// Release a table, its entries, their label lists, their edges, the bank
// indexes and the label definitions.  Nothing in the parse tree or the
// symbol tables is touched; the table only ever pointed into them.
void
optFreeTable(OptTableP tableP)
{
int i;
OptLabelDefP defP;
OptLabelDefP nextDefP;

    if( !tableP )
    {
        return;
    }

    for( i = 0; i < tableP->count; ++i )
    {
        freeLabelList(tableP->entriesPP[i]->labelsP);
        freeEdgeList(tableP->entriesPP[i]->outP);
        free(tableP->entriesPP[i]);
    }

    free(tableP->entriesPP);

    for( i = 0; i <= MAXBANK; ++i )
    {
        if( tableP->banksP[i] )
        {
            free(tableP->banksP[i]);
        }
    }

    for( defP = tableP->labelsP; defP; defP = nextDefP )
    {
        nextDefP = defP->nextP;
        free(defP);
    }

    // Task A4's blocks and their successor edges.  The entries are already
    // gone, but a block owns nothing of theirs: only its own edge list.
    freeBlockList(tableP);

    // Task A5's findings.  A finding owns its two strings and nothing else;
    // the words it names belong to the table and are already gone.
    freeFindingList(tableP->findingsP);
    freeFindingList(tableP->suppressedP);

    free(tableP);
}

// Allocate an empty table with room for INITIAL_CAPACITY entries.
// Returns the table; allocation failure is fatal.
static OptTableP
newTable(void)
{
OptTableP tableP;

    if( !(tableP = (OptTableP)calloc(1, sizeof(OptTable))) )
    {
        fprintf(stderr, "am1: out of memory building the optimizer word table\n");
        exit(1);
    }

    tableP->capacity = INITIAL_CAPACITY;

    if( !(tableP->entriesPP = (OptWordP *)calloc(tableP->capacity, sizeof(OptWordP))) )
    {
        fprintf(stderr, "am1: out of memory building the optimizer word table\n");
        exit(1);
    }

    return(tableP);
}

// Append one entry to the table and index it.  The bank and address are
// range checked here, once, so nothing downstream has to; an entry outside
// the machine marks the build failed and is not added.
// Returns the new entry, or NILP if it was out of range.
static OptWordP
addEntry(OptTableP tableP, int bank, int addr, int value, OptKind kind,
    PNodeP nodeP, PNodeP exprP, SymNodeP symP, int lineNo, unsigned int flags)
{
OptWordP entryP;
OptWordP *newPP;

    if( (bank < 0) || (bank > MAXBANK) || (addr < 0) || (addr >= BANKSIZE) )
    {
        fprintf(stderr, "am1: optimizer: word at bank %d address 0%o is outside memory (line %d)\n",
            bank, addr, lineNo);
        builder.failed = true;
        return(NILP);
    }

    if( tableP->count >= tableP->capacity )
    {
        // Double the entry array; the entries themselves do not move.
        tableP->capacity = (tableP->capacity * 2);

        if( !(newPP = (OptWordP *)realloc(tableP->entriesPP, (tableP->capacity * sizeof(OptWordP)))) )
        {
            fprintf(stderr, "am1: out of memory building the optimizer word table\n");
            exit(1);
        }

        tableP->entriesPP = newPP;
    }

    if( !(entryP = (OptWordP)calloc(1, sizeof(OptWord))) )
    {
        fprintf(stderr, "am1: out of memory building the optimizer word table\n");
        exit(1);
    }

    entryP->bank = bank;
    entryP->addr = addr;
    entryP->value = (value & WRDMASK);
    entryP->kind = kind;
    entryP->nodeP = nodeP;
    entryP->exprP = exprP;
    entryP->symP = symP;
    entryP->fileP = builder.fileP;
    entryP->lineNo = lineNo;
    entryP->flags = flags;
    entryP->index = tableP->count;
    entryP->labelsP = NILP;
    entryP->sameAddrP = NILP;

    tableP->entriesPP[tableP->count++] = entryP;

    if( flags & OPTF_RESERVED )
    {
        ++tableP->reservedCount;
    }

    indexEntry(tableP, entryP);
    checkPlacement(tableP, entryP);
    return(entryP);
}

// Put an entry into its bank's per-address index.  The first entry at an
// address is pointed to directly; a later one at the same address (an
// overlay, which only assembles under -M) is chained behind it through
// sameAddrP and every entry in the chain is flagged OPTF_DUPADDR.
static void
indexEntry(OptTableP tableP, OptWordP entryP)
{
OptBankP bankP;
OptWordP chainP;

    if( !(bankP = tableP->banksP[entryP->bank]) )
    {
        if( !(bankP = (OptBankP)calloc(1, sizeof(OptBank))) )
        {
            fprintf(stderr, "am1: out of memory building the optimizer word table\n");
            exit(1);
        }

        bankP->bank = entryP->bank;
        tableP->banksP[entryP->bank] = bankP;
    }

    ++bankP->count;

    if( !bankP->wordsP[entryP->addr] )
    {
        bankP->wordsP[entryP->addr] = entryP;
        return;
    }

    // Walk to the end of the chain, then flag the whole chain.  The head was
    // not a duplicate until now, so it may not be flagged yet.
    for( chainP = bankP->wordsP[entryP->addr]; chainP->sameAddrP; chainP = chainP->sameAddrP )
    {
        ;
    }

    chainP->sameAddrP = entryP;

    for( chainP = bankP->wordsP[entryP->addr]; chainP; chainP = chainP->sameAddrP )
    {
        if( !(chainP->flags & OPTF_DUPADDR) )
        {
            chainP->flags |= OPTF_DUPADDR;
            ++tableP->dupCount;
        }
    }
}

// Record a label definition for later attachment.  Labels are attached
// after the walk rather than during it because a label on a line of its own
// ("foo," then the word on the next line) names whatever is emitted at its
// address, and that may be a word from a later statement or a pool.
static void
addLabelDef(OptTableP tableP, SymNodeP symP, int bank, int addr)
{
OptLabelDefP defP;

    if( !(defP = (OptLabelDefP)calloc(1, sizeof(OptLabelDef))) )
    {
        fprintf(stderr, "am1: out of memory building the optimizer word table\n");
        exit(1);
    }

    defP->symP = symP;
    defP->bank = bank;
    defP->addr = addr;
    defP->attached = 0;
    defP->nextP = tableP->labelsP;
    tableP->labelsP = defP;
    ++tableP->labelCount;
}

// Attach every recorded label to every entry at its bank and address.  An
// overlaid address gets the label on each of its entries, since the label
// names the address, not one emission of it.  A label with no entry at its
// address (one at the very end of the program, say) stays on the table's
// definition list with attached still zero.
static void
attachLabels(OptTableP tableP)
{
OptLabelDefP defP;
OptBankP bankP;
OptWordP entryP;
OptLabelP labelP;

    for( defP = tableP->labelsP; defP; defP = defP->nextP )
    {
        if( (defP->bank < 0) || (defP->bank > MAXBANK) || (defP->addr < 0) || (defP->addr >= BANKSIZE) )
        {
            continue;
        }

        if( !(bankP = tableP->banksP[defP->bank]) )
        {
            continue;
        }

        for( entryP = bankP->wordsP[defP->addr]; entryP; entryP = entryP->sameAddrP )
        {
            if( !(labelP = (OptLabelP)calloc(1, sizeof(OptLabel))) )
            {
                fprintf(stderr, "am1: out of memory building the optimizer word table\n");
                exit(1);
            }

            labelP->symP = defP->symP;
            labelP->nextP = entryP->labelsP;
            entryP->labelsP = labelP;
            defP->attached = 1;
        }
    }
}

// Free one entry's label list.
static void
freeLabelList(OptLabelP labelP)
{
OptLabelP nextP;

    while( labelP )
    {
        nextP = labelP->nextP;
        free(labelP);
        labelP = nextP;
    }
}

// Walk the statement list and add every word it emits.  This is the walk
// from writeStatements() in bincodegen.c with the output replaced by table
// entries and the memory-overwrite checks left out (the binary generator
// still makes them; here an overwrite just becomes an overlay chain).
static void
walkStatements(OptTableP tableP, PNodeP nodeP)
{
    while( nodeP && !builder.failed )
    {
        switch( nodeP->type )
        {
        case COMMENT:           // none of these emit anything or change state
        case TERMINATOR:
            break;

        case ORIGIN:
            // "expr/" moves the pc.  The node never carries a word of its
            // own (the parser leaves rightP empty) but the generators check
            // anyway, so this does too.
            builder.pc = (nodeP->value.ival & ADDRMASK);

            if( canReduce(nodeP->rightP) )
            {
                addExprWord(tableP, nodeP, nodeP->rightP);
            }
            break;

        case EXPR:
            if( canReduce(nodeP->rightP) )
            {
                addExprWord(tableP, nodeP, nodeP->rightP);
            }
            break;

        case LOCATION:
        case LCLLOCATION:
            // The label itself, at the node's pc, whatever follows it.
            addLabelDef(tableP, nodeP->value.symP, nodeP->bank, nodeP->pc);

            if( canReduce(nodeP->rightP) )
            {
                // A single word on the same line as the label ("foo, jmp bar").
                addExprWord(tableP, nodeP, nodeP->rightP);
            }
            else if( nodeP->rightP )
            {
                // A text directive on the same line as the label
                // ("msg, text \"hello\""), which emits several words.
                switch( nodeP->rightP->type )
                {
                case TEXT:
                    addTextWords(tableP, nodeP, nodeP->rightP, OPTK_TEXT);
                    break;

                case TYPE340:
                    addTextWords(tableP, nodeP, nodeP->rightP, OPTK_TYPE340);
                    break;

                case ASCII:
                    addAsciiWords(tableP, nodeP, nodeP->rightP);
                    break;

                default:
                    // A directive that emits nothing (local, endloc and the like).
                    break;
                }
            }
            break;

        case VARS:
            // An explicit 'variables' directive: the list of every variable
            // declared in this bank so far, addresses already assigned.
            addVarWords(tableP, (PNodeListP)(nodeP->value.ptr), nodeP->bank);
            break;

        case CONSTANTS:
            // An explicit 'constants' directive: the pool gathered so far in
            // this bank.  The parser leaves value.symP empty when there was
            // nothing to pool, and addConstWords() accepts that.
            addConstWords(tableP, nodeP->value.symP, nodeP->bank);
            break;

        case TEXT:
            addTextWords(tableP, nodeP, nodeP, OPTK_TEXT);
            break;

        case TYPE340:
            addTextWords(tableP, nodeP, nodeP, OPTK_TYPE340);
            break;

        case ASCII:
            addAsciiWords(tableP, nodeP, nodeP);
            break;

        case BANK:
            // Switch banks; value2 holds the pc the new bank continues at.
            builder.bank = nodeP->value.ival;
            builder.pc = (nodeP->value2.ival & ADDRMASK);
            break;

        case FILENAME:
            // A cpp line marker the lexer turned into a statement.  The words
            // that follow it come from this file and their line numbers are
            // that file's, so remembering the name is the only way a finding
            // can say which file to look in.  Nothing is emitted.
            builder.fileP = nodeP->value.strP;
            break;

        case TABLE:
            addTableWords(tableP, nodeP);
            break;

        default:
            // Everything else (var declarations, export, import, comments of
            // every flavour, file markers) emits nothing.
            break;
        }

        nodeP = nodeP->leftP;
    }
}

// Add the one word of an expression statement: an EXPR node, or a LOCATION
// or LCLLOCATION node with an expression on the label's line.  The word's
// address is the node's own pc; the value is the expression reduced the way
// the binary generator reduces it.
static void
addExprWord(OptTableP tableP, PNodeP nodeP, PNodeP exprP)
{
int value;

    value = reduceOperand(exprP);
    addEntry(tableP, nodeP->bank, nodeP->pc, value, OPTK_EXPR, nodeP, exprP, NILP, nodeP->lineNo, 0);
    advancePC(1);
}

// Add the words of a packed 'text' or 'type340' string.  nodeP is the
// statement node the words are credited to (the LOCATION node when the
// string shares a line with a label), textNodeP the node carrying the
// FlexText.  The packing is writeText()'s from bincodegen.c: three six-bit
// characters per word, high character first, a short final word padded on
// the right with zero characters, and an empty string still emitting one
// zero word.  writeText() never clears its accumulator between words, so
// every word after the first carries the earlier characters above bit 17;
// the tape writer drops those bits and so does this, by masking as it goes.
static void
addTextWords(OptTableP tableP, PNodeP nodeP, PNodeP textNodeP, OptKind kind)
{
int i;
int val;
int addr;
char *bufP;
FlexText flexText;

    flexText = textNodeP->value.flexText;
    bufP = flexText.bufP;
    addr = textNodeP->pc;

    for( val = i = 0; i < flexText.nchars; i++ )
    {
        if( i && !(i % 3) )
        {
            addEntry(tableP, textNodeP->bank, addr++, val, kind, nodeP, NILP, NILP, textNodeP->lineNo, 0);
            advancePC(1);
        }

        val = (((val << 6) | *bufP++) & WRDMASK);
    }

    if( i % 3 )     // a partial last word, pad it out
    {
        while( i++ % 3 )
        {
            val = ((val << 6) & WRDMASK);
        }

        addEntry(tableP, textNodeP->bank, addr++, val, kind, nodeP, NILP, NILP, textNodeP->lineNo, 0);
        advancePC(1);
    }
    else if( (i >= flexText.nchars) && !(i % 3) )
    {
        // The last full word, or the single zero word of an empty string.
        addEntry(tableP, textNodeP->bank, addr++, val, kind, nodeP, NILP, NILP, textNodeP->lineNo, 0);
        advancePC(1);
    }
}

// Add the words of a packed 'ascii' string.  nodeP is the statement node the
// words are credited to, asciiNodeP the node carrying the string.  The
// packing is writeAscii()'s from bincodegen.c: two nine-bit characters per
// word, high character first, the terminating NUL included, and a final
// lone character padded with a zero low byte.
static void
addAsciiWords(OptTableP tableP, PNodeP nodeP, PNodeP asciiNodeP)
{
int i;
int word;
int addr;
char *strP;

    strP = asciiNodeP->value.strP;
    addr = asciiNodeP->pc;
    word = 0;
    i = 0;      // 0 is doing the high byte, 1 the low byte

    do
    {
        if( !i )
        {
            word = *strP;
        }
        else
        {
            word = ((word << 9) | *strP);
            addEntry(tableP, asciiNodeP->bank, addr++, word, OPTK_ASCII, nodeP, NILP, NILP, asciiNodeP->lineNo, 0);
            advancePC(1);
        }

        i ^= 1;
    }
    while( *strP++ );

    if( i )         // a high byte with no low byte to pair it with
    {
        addEntry(tableP, asciiNodeP->bank, addr++, (word << 9), OPTK_ASCII, nodeP, NILP, NILP, asciiNodeP->lineNo, 0);
        advancePC(1);
    }
}

// Add the words of a 'table' directive.  With an initializer every word gets
// the initializer's value, evaluated once, as the binary generator does.
// Without one the words are reserved: they occupy memory, nothing is
// emitted for them, and the entries are flagged OPTF_RESERVED so the dump
// skips them and the analyses know the addresses are data.
static void
addTableWords(OptTableP tableP, PNodeP nodeP)
{
int i;
int value;
unsigned int flags;

    if( nodeP->rightP )
    {
        value = evalExpr(nodeP->rightP);
        flags = 0;
    }
    else
    {
        value = 0;
        flags = OPTF_RESERVED;
    }

    for( i = 0; (i < nodeP->value.ival) && !builder.failed; ++i )
    {
        addEntry(tableP, nodeP->bank, (nodeP->pc + i), value, OPTK_TABLE, nodeP, nodeP->rightP, NILP,
            nodeP->lineNo, flags);
        advancePC(1);
    }
}

// Add one word per variable on a var list.  The list is the bank's chain of
// declarations, most recent first, which is the order every generator emits
// it in.  Each list item's node is the variable's ADDR node: its symbol holds
// the assigned address, its leftP the initializer if there was one.  The
// variable's name is also a label at its word.
static void
addVarWords(OptTableP tableP, PNodeListP listP, int bank)
{
int value;
PNodeP nodeP;
SymNodeP symP;

    while( listP && !builder.failed )
    {
        nodeP = listP->nodeP;
        symP = nodeP->value.symP;

        value = (nodeP->leftP)?reduceOperand(nodeP->leftP):0;
        addEntry(tableP, bank, nodeP->pc, value, OPTK_VAR, nodeP, nodeP->leftP, symP, nodeP->lineNo, 0);
        addLabelDef(tableP, symP, bank, nodeP->pc);
        advancePC(1);

        listP = listP->nextP;
    }
}

// Add one word per constant in a pool.  A pool is a symbol tree keyed by the
// expression's fingerprint; the parser assigned each entry's address in a
// pre-order walk (this node, left subtree, right subtree) and the generators
// emit in the same order, so this walks the same way.  The symbol's value is
// its address, value2 the evaluated constant, ptr the expression that first
// interned the slot.  The pool symbol is also recorded as a label at its
// word: its name is the fingerprint, not anything the source wrote, but it
// is the only handle the CONSTANT reference nodes carry.
static void
addConstWords(OptTableP tableP, SymNodeP symP, int bank)
{
PNodeP exprP;
int lineNo;

    if( !symP || builder.failed )
    {
        return;
    }

    exprP = (PNodeP)(symP->ptr);
    lineNo = (exprP)?exprP->lineNo:-1;

    addEntry(tableP, bank, symP->value, symP->value2, OPTK_CONST, NILP, exprP, symP, lineNo, 0);
    addLabelDef(tableP, symP, bank, symP->value);
    advancePC(1);

    addConstWords(tableP, symP->leftP, bank);
    addConstWords(tableP, symP->rightP, bank);
}

// Decide whether an expression statement emits a word, exactly as the code
// generators decide it.  Directives that parse as expressions (local,
// private, endloc and their kin) emit nothing, and neither do the text
// directives, which the LOCATION handler deals with separately.
// Returns 1 if the expression emits a word, 0 if it does not.
static int
canReduce(PNodeP nodeP)
{
    if( !nodeP )
    {
        return(0);
    }

    switch( nodeP->type )
    {
    case ORIGIN:
    case LOCAL:
    case PRIVATE:
    case ADDLOCAL:
    case ENDLOC:
    case FORCELOC:
    case TERMINATOR:
        return(0);

    case TEXT:
    case ASCII:
    case TYPE340:
        return(0);

    case EXPR:
    case SEPARATOR:
        return( canReduce(nodeP->rightP) );

    default:
        return(1);
    }
}

// Reduce a statement's expression to its word, the way reduceOperand() in
// bincodegen.c does: a bare '.' at the top of the expression is the address
// the word is being written at, everything else goes through evalExpr(),
// which already resolves a '.' nested inside an expression from the value
// the parser snapshotted when it built the node.
// Returns the 18-bit word.
static int
reduceOperand(PNodeP nodeP)
{
    if( !nodeP )
    {
        return(0);
    }

    if( nodeP->type == DOT )
    {
        return( builder.pc );
    }

    return( evalExpr(nodeP) );
}

// Advance the running pc as adjustPC() in bincodegen.c does, wrapping to 0
// when it runs off the end of the bank.
static void
advancePC(int incr)
{
    builder.pc = (builder.pc + incr);

    if( builder.pc > ADDRMASK )
    {
        builder.pc = 0;
    }
}

// Compare where the parser said a word goes (the entry's bank and address,
// taken from the node or the symbol) with where the binary generator's
// running pc would put it.  A difference is a parser defect, not something
// the optimizer can resolve; the entry is flagged and counted so the report
// can say so.
static void
checkPlacement(OptTableP tableP, OptWordP entryP)
{
    if( (entryP->bank != builder.bank) || (entryP->addr != builder.pc) )
    {
        entryP->flags |= OPTF_PCMISMATCH;
        ++tableP->mismatchCount;
    }
}

// Write the phase 1 report (study section 7).  The order is task A5's: the
// heading and the notes that say what the reader is looking at, then the
// findings, then the patterns a precondition or a rule refused, then the
// classification conflicts and the apparently unreached code from tasks A3
// and A4, then the statistics.  A reader who stops after the second section
// has the actionable half; everything after it is evidence and background.
static void
writeReport(FILE *fP, OptTableP tableP, char *basenameP)
{
    fprintf(fP, "am1 optimizer report for %s\n", basenameP);
    fprintf(fP, "%s\n", AM1VERSION);
    fprintf(fP, "source file %s\n", (origFilenameP)?origFilenameP:basenameP);

    writeHeaderNotes(fP);
    writeFindingList(fP, tableP);
    writeSuppressedList(fP, tableP);
    writeConflictList(fP, tableP);
    writeUnreachedList(fP, tableP);
    writeStatistics(fP, tableP);
}

// Give a kind its name for the report.
// Returns a static string, never NILP.
static const char *
kindName(OptKind kind)
{
    switch( kind )
    {
    case OPTK_EXPR:
        return("expr");

    case OPTK_TEXT:
        return("text");

    case OPTK_ASCII:
        return("ascii");

    case OPTK_TYPE340:
        return("type340");

    case OPTK_TABLE:
        return("table");

    case OPTK_VAR:
        return("var");

    case OPTK_CONST:
        return("const");

    default:
        return("unknown");
    }
}

// ===========================================================================
// Task A2: the instruction decoder, the spelling classifier, the operate
// group phase table, the decoded dump and the self-check.
//
// The decoder is deliberately dumb: it decodes every word's bits the same
// way whether the word is an instruction, a pointer, a constant or a packed
// character triple, and leaves the code/data judgement to task A3.  Its
// second half looks at the expression tree the word came from and records
// the shape the source used, which is the evidence A3 weighs.
// ===========================================================================

// The operate-group phase table.  The phases and the reasoning behind them
// are documented at OptPhase in optimizer.h.  The handbook gives the shape
// (F-15D page 21, "The instruction opr 3200 will clear the AC, put TW to AC,
// and complement AC"; page 22 for hlt, cla, clf, stf and nop) and the
// emulator's cycle0() gives the time pulse of every micro-op, PDP-1D ones
// included (read on the owner's direction 08-Sep-2026; the finding is in
// Claude/skill-updates/pdp1-operate-timing.md):
//
//   TP7   cla, cli                 the clears
//   TP8   cmi, lat, lap, clf/stf   IO complement, ORs into AC, the flag field;
//                                  lai and lia are armed here
//   TP9   cma, hlt                 AC complement, then the stop
//   next  lia, lai                 the transfer completes in the next
//                                  instruction's fetch: lia alone is IO <- AC,
//                                  lai alone AC <- IO, both at once the exchange
//
// Within a phase the order column separates rows only where it matters:
// cma before hlt for the listing's sake (hlt touches no register), lia and
// lai equal because together they are one exchange.  clf and stf touch
// nothing another micro-op reads, so their phase never decides a merge.
const OptMicroOp optMicroOps[OPTMO_COUNT] =
{
    { OPTMO_CLA, "cla", OPTM_CLA,       OPTPH_CLEAR,      0, OPTE_WRITES_AC,                   0 },
    { OPTMO_CLI, "cli", OPTM_CLI,       OPTPH_CLEAR,      0, OPTE_WRITES_IO,                   0 },
    { OPTMO_CLF, "clf", OPTM_FLAGFIELD, OPTPH_TRANSFER,   0, OPTE_WRITES_FLAGS,                0 },
    { OPTMO_STF, "stf", OPTM_FLAGFIELD, OPTPH_TRANSFER,   0, OPTE_WRITES_FLAGS,                0 },
    { OPTMO_LAT, "lat", OPTM_LAT,       OPTPH_TRANSFER,   0, (OPTE_READS_AC | OPTE_WRITES_AC), 0 },
    { OPTMO_LAP, "lap", OPTM_LAP,       OPTPH_TRANSFER,   0, (OPTE_READS_AC | OPTE_WRITES_AC), 0 },
    { OPTMO_CMA, "cma", OPTM_CMA,       OPTPH_COMPLEMENT, 0, (OPTE_READS_AC | OPTE_WRITES_AC), 0 },
    { OPTMO_HLT, "hlt", OPTM_HLT,       OPTPH_COMPLEMENT, 1, OPTE_HALTS,                       0 },
    { OPTMO_CMI, "cmi", OPTM_CMI,       OPTPH_TRANSFER,   0, (OPTE_READS_IO | OPTE_WRITES_IO), 1 },
    { OPTMO_LIA, "lia", OPTM_LIA,       OPTPH_EXCHANGE,   0, (OPTE_READS_AC | OPTE_WRITES_IO), 1 },
    { OPTMO_LAI, "lai", OPTM_LAI,       OPTPH_EXCHANGE,   0, (OPTE_READS_IO | OPTE_WRITES_AC), 1 }
};

// Memory reference mnemonics by instruction code, indexed by the code with
// bit 5 dropped (code >> 1, so and = 02 is index 1).  Values from
// permsyms.def; the handbook's list on page 67 agrees.  Code 12 is among the
// spares: permsyms.def defined jfd there until 8-Sep-2026 and the owner has
// since ruled the code unused.  Code 16 is cal, and
// the same code with bit 5 set is jda (page 18: "The jda instruction
// requires that the indirect bit be a one, but indirect addressing does not
// occur"); decodeMemref() sorts that out.  The spare codes and the codes that
// are groups of their own are NILP.
static const char *memrefNames[32] =
{
    NILP,  "and", "ior", "xor", "xct", NILP,  NILP,  "cal",    // codes 00 to 16
    "lac", "lio", "dac", "dap", "dip", "dio", "dzm", NILP,     // codes 20 to 36
    "add", "sub", "idx", "isp", "sad", "sas", "mul", "div",    // codes 40 to 56
    "jmp", "jsp", NILP,  NILP,  NILP,  NILP,  NILP,  NILP      // codes 60 to 76
};

// Shift group mnemonics, indexed [shift][right][registers] (handbook page
// 19).  A word with no register selected has no mnemonic; the handbook's
// group name sft stands in.
static const char *shiftNames[2][2][4] =
{
    { { "sft", "ral", "ril", "rcl" }, { "sft", "rar", "rir", "rcr" } },
    { { "sft", "sal", "sil", "scl" }, { "sft", "sar", "sir", "scr" } }
};

// The single-bit skip conditions in the order the dump prints them, highest
// bit first.
static const struct
{
    unsigned int bit;
    const char *nameP;
} skipCondNames[] =
{
    { OPTC_SNI, "sni" },
    { OPTC_SPI, "spi" },
    { OPTC_SZO, "szo" },
    { OPTC_SMA, "sma" },
    { OPTC_SPA, "spa" },
    { OPTC_SZA, "sza" }
};

// The special operate (code 74) micro-ops in the order the dump prints them.
static const struct
{
    unsigned int bit;
    const char *nameP;
} specialNames[] =
{
    { OPTX_IIF, "iif" },
    { OPTX_IFI, "ifi" },
    { OPTX_IDA, "ida" },
    { OPTX_SCI, "sci" },
    { OPTX_SCF, "scf" }
};

// The operate micro-ops in the order the dump prints them, highest bit
// first; the flag field is printed after these.
static const OptMicroId operateDumpOrder[] =
{
    OPTMO_CMI, OPTMO_CLI, OPTMO_LAT, OPTMO_CMA, OPTMO_HLT, OPTMO_CLA, OPTMO_LAP, OPTMO_LAI, OPTMO_LIA
};

// What the spelling scanner counts while it walks one word's expression.
typedef struct
{
    int lawCount;           // LAW nodes
    int opaddrCount;        // OPADDR nodes: mnemonics that take an address
    int opcodeCount;        // OPCODE and OPORABLE nodes: mnemonics that do not
    int oneDCount;          // of those, symbols flagged SYMF_1DOP
    int imodCount;          // i
    int modifierCount;      // VALUESPEC: 1s to 9s, C
    int numberCount;        // integers, characters, flexo codes, '.'
    int addrSymCount;       // ADDR, LCLADDR, BREF, WILDREF
    int constRefCount;      // [..]
    int arithCount;         // arithmetic operators at the top level of the word
    int otherCount;         // node types the scanner does not know
    int opInArith;          // an opcode-class symbol inside arithmetic
    SymNodeP firstOpSymP;   // the first opcode-class symbol in source order
} Shape, *ShapeP;

extern SymNodeP permSymP;   // the permanent symbols, from permsyms.def.c

static int bitCount(unsigned int bits);
static const char *findPermOpcodeName(SymNodeP symP, int value);
static void scanShape(PNodeP nodeP, ShapeP shapeP, int inArith);
static void noteOpSym(ShapeP shapeP, SymNodeP symP);
static void printGroup(FILE *fP, OptDecodeP decodeP);
static void printSpellingNames(FILE *fP, PNodeP nodeP);
static void checkResult(FILE *fP, const char *whatP, int passed, int *passedP, int *totalP);

// Decode one 18-bit value.  Every group's fields are filled from the bits
// alone; nothing here looks at the source.
void
optDecodeValue(int value, OptDecodeP decodeP)
{
int op6;
int i;

    memset(decodeP, 0, sizeof(OptDecode));
    value = (value & WRDMASK);

    // The six-bit code as the handbook writes it (two octal digits), split
    // into the five-bit instruction code and bit 5.
    op6 = ((value >> 12) & 077);
    decodeP->opcode = (op6 & 076);
    decodeP->indirect = (op6 & 01);
    decodeP->address = (value & ADDRMASK);

    switch( decodeP->opcode )
    {
    case 064:
        // Skip group: the single-bit conditions, the switch and flag
        // selectors, and bit 5 reversing the sense to "do not skip if".
        decodeP->group = OPTG_SKIP;
        decodeP->skipConds = (decodeP->address & OPTC_CONDMASK);
        decodeP->skipSwitch = ((decodeP->address & OPTC_SZSMASK) >> 3);
        decodeP->skipFlag = (decodeP->address & OPTC_SZFMASK);
        decodeP->skipInverted = decodeP->indirect;
        decodeP->uses1D = ((decodeP->skipConds & OPTC_SNI) != 0);
        break;

    case 066:
        // Shift group: bit 5 direction, bit 6 shift against rotate, bits 7-8
        // the registers, the count is the number of ones in bits 9-17.
        decodeP->group = OPTG_SHIFT;
        decodeP->shiftRight = decodeP->indirect;
        decodeP->shiftArith = ((decodeP->address & OPTSH_ARITH) != 0);
        decodeP->shiftRegs = ((decodeP->address & OPTSH_REGS) >> 9);
        decodeP->shiftCount = bitCount(decodeP->address & OPTSH_COUNT);
        decodeP->mnemonicP = shiftNames[decodeP->shiftArith][decodeP->shiftRight][decodeP->shiftRegs];
        break;

    case 070:
        // law N; bit 5 loads -N instead (handbook page 18).
        decodeP->group = OPTG_LAW;
        decodeP->mnemonicP = "law";
        break;

    case 072:
        // In-out transfer.  Named only when the whole value is one of the
        // iot mnemonics permsyms.def defines; otherwise it is just "iot".
        decodeP->group = OPTG_IOT;
        decodeP->iotWait = decodeP->indirect;
        decodeP->iotComplete = ((decodeP->address & OPTIO_COMPLETE) != 0);
        decodeP->iotSub = ((decodeP->address & OPTIO_SUBMASK) >> 6);
        decodeP->iotDevice = (decodeP->address & OPTIO_DEVMASK);
        decodeP->mnemonicP = findPermOpcodeName(permSymP, value);

        if( !decodeP->mnemonicP )
        {
            decodeP->mnemonicP = "iot";
        }
        break;

    case 074:
        // The PDP-1D special operate group; a spare (halting) code on a
        // plain PDP-1, but the 1D decode is always on.
        decodeP->group = OPTG_1D;
        decodeP->specialBits = (decodeP->address & OPTX_ALLBITS);
        decodeP->uses1D = 1;
        break;

    case 076:
        // Operate group: the micro-op bits are the word's low 13 bits.
        decodeP->group = OPTG_OPERATE;
        decodeP->microBits = (value & OPTM_ALLBITS);
        decodeP->flagNum = (decodeP->microBits & OPTM_FLAGNUM);

        if( decodeP->microBits & OPTM_STF )
        {
            decodeP->flagOp = OPTFLAG_STF;
        }
        else if( decodeP->flagNum )
        {
            decodeP->flagOp = OPTFLAG_CLF;
        }
        else
        {
            decodeP->flagOp = OPTFLAG_NONE;
        }

        decodeP->uses1D = ((decodeP->microBits & (OPTM_CMI | OPTM_LIA | OPTM_LAI)) != 0);
        break;

    default:
        // A memory reference, or a spare code.
        i = (decodeP->opcode >> 1);

        if( memrefNames[i] )
        {
            decodeP->group = OPTG_MEMREF;
            decodeP->mnemonicP = memrefNames[i];
            decodeP->memIndirect = decodeP->indirect;

            // cal with bit 5 set is jda, and jda never indirects.
            if( (decodeP->opcode == 016) && decodeP->indirect )
            {
                decodeP->mnemonicP = "jda";
                decodeP->memIndirect = 0;
            }
        }
        else
        {
            decodeP->group = OPTG_UNKNOWN;
        }
        break;
    }
}

// Classify how an expression tree spells its word.  The tree is scanned
// once, counting what kinds of leaves it has and whether any opcode-class
// symbol sits inside arithmetic; the spelling follows from the counts.  The
// rules, in the order they are applied:
//
//   a [..] anywhere                          -> CONSTREF
//   an opcode symbol inside arithmetic       -> OTHER   (jmp+foo)
//   law, alone or with operands              -> LAW     (but law with another mnemonic is OTHER)
//   no opcode symbol at all                  -> DATA
//   one address-taking mnemonic (OPADDR):
//       with any operand                     -> MNEMONIC_OPERAND   (lac x, jmp .+2, dac i p)
//       with none, or only i                 -> MNEMONIC_ONLY      (cal, lac)
//       with another mnemonic                -> OTHER              (jmp x cla, lac dac)
//   only OPCODE/OPORABLE mnemonics:
//       with no address symbol               -> MNEMONIC_ONLY      (sza i, cla cma, ral 3s, szf 7, opr 3200)
//       with an address symbol               -> OTHER              (sza x)
//   anything the scanner did not recognize   -> OTHER
//
// A modifier (i, 1s to 9s, C) is never an operand: "lac i" is MNEMONIC_ONLY
// and "ral 3s" is MNEMONIC_ONLY, while "ral 3" is also MNEMONIC_ONLY because
// ral is an OPCODE, not an OPADDR.
void
optClassifySpelling(PNodeP exprP, OptDecodeP decodeP)
{
Shape shape;
int operandCount;

    decodeP->spelling = OPTS_NONE;
    decodeP->spelled1D = 0;
    decodeP->spelledIndirect = 0;
    decodeP->hasConstRef = 0;
    decodeP->opSymCount = 0;
    decodeP->opSymP = NILP;

    if( !exprP )
    {
        return;
    }

    memset(&shape, 0, sizeof(shape));
    scanShape(exprP, &shape, 0);

    decodeP->spelled1D = (shape.oneDCount > 0);
    decodeP->spelledIndirect = (shape.imodCount > 0);
    decodeP->hasConstRef = (shape.constRefCount > 0);
    decodeP->opSymCount = (shape.lawCount + shape.opaddrCount + shape.opcodeCount);
    decodeP->opSymP = shape.firstOpSymP;

    // Anything that is not a mnemonic or a modifier counts as an operand.
    operandCount = (shape.numberCount + shape.addrSymCount + shape.arithCount + shape.constRefCount);

    if( shape.constRefCount )
    {
        decodeP->spelling = OPTS_CONSTREF;
    }
    else if( shape.opInArith || shape.otherCount )
    {
        decodeP->spelling = OPTS_OTHER;
    }
    else if( shape.lawCount )
    {
        if( (shape.lawCount > 1) || shape.opaddrCount || shape.opcodeCount )
        {
            decodeP->spelling = OPTS_OTHER;
        }
        else
        {
            decodeP->spelling = OPTS_LAW;
        }
    }
    else if( !shape.opaddrCount && !shape.opcodeCount )
    {
        decodeP->spelling = OPTS_DATA;
    }
    else if( shape.opaddrCount )
    {
        if( (shape.opaddrCount > 1) || shape.opcodeCount )
        {
            decodeP->spelling = OPTS_OTHER;
        }
        else if( operandCount )
        {
            decodeP->spelling = OPTS_MNEMONIC_OPERAND;
        }
        else
        {
            decodeP->spelling = OPTS_MNEMONIC_ONLY;
        }
    }
    else
    {
        if( shape.addrSymCount )
        {
            decodeP->spelling = OPTS_OTHER;
        }
        else
        {
            decodeP->spelling = OPTS_MNEMONIC_ONLY;
        }
    }
}

// Decode one table entry: the value, then the spelling.  A reserved table
// word has no value to decode; it is left as OPTG_UNKNOWN with OPTS_NONE.
void
optDecodeWord(OptWordP entryP)
{
    if( entryP->flags & OPTF_RESERVED )
    {
        memset(&entryP->decode, 0, sizeof(OptDecode));
        return;
    }

    optDecodeValue(entryP->value, &entryP->decode);
    optClassifySpelling(entryP->exprP, &entryP->decode);
}

// Decode every entry of a table.
void
optDecodeTable(OptTableP tableP)
{
int i;

    for( i = 0; i < tableP->count; ++i )
    {
        optDecodeWord(tableP->entriesPP[i]);
    }
}

// Name a group for the report and the dump.
// Returns a static string, never NILP.
const char *
optGroupName(OptGroup group)
{
    switch( group )
    {
    case OPTG_MEMREF:
        return("memref");

    case OPTG_LAW:
        return("law");

    case OPTG_SKIP:
        return("skip");

    case OPTG_SHIFT:
        return("shift");

    case OPTG_OPERATE:
        return("operate");

    case OPTG_IOT:
        return("iot");

    case OPTG_1D:
        return("1d");

    case OPTG_UNKNOWN:
    default:
        return("unknown");
    }
}

// Name a spelling for the report and the dump.
// Returns a static string, never NILP.
const char *
optSpellingName(OptSpelling spelling)
{
    switch( spelling )
    {
    case OPTS_MNEMONIC_OPERAND:
        return("mnemonic_operand");

    case OPTS_MNEMONIC_ONLY:
        return("mnemonic_only");

    case OPTS_LAW:
        return("law");

    case OPTS_CONSTREF:
        return("constref");

    case OPTS_DATA:
        return("data");

    case OPTS_OTHER:
        return("other");

    case OPTS_NONE:
    default:
        return("none");
    }
}

// Print the decoded dump (-O=decode), one line per entry in emission order,
// reserved words included:
//
//   BBAAAA VVVVVV kind    group and fields | spelling names [1d]
//
// BBAAAA is the bank and address as in -T, VVVVVV the value (------ for a
// reserved word), kind the OptKind name padded to seven characters.  The
// group text is:
//
//   memref MNEM [i] AAAA     the mnemonic, i when the word really indirects
//                            (jda never does), the 12-bit address
//   law [i] NNNN             i when the immediate is negated
//   skip COND... [szsN] [szfN] [i]
//                            conditions highest bit first (sni spi szo sma
//                            spa sza), then the switch and flag selectors,
//                            then i for a reversed sense; "skip skp" when the
//                            word selects no condition at all
//   shift MNEM N             the mnemonic (sft when no register is selected)
//                            and the step count
//   operate OP... [clfN|stfN]
//                            micro-ops highest bit first (cmi cli lat cma hlt
//                            cla lap lai lia), then the flag operation;
//                            "operate nop" when no bit is set
//   iot NAME [wait] [cpl] [sub NN] dev NN
//                            the permsyms.def name when the value is exactly
//                            one of them, else iot; the bit 5 wait, the bit 6
//                            completion bit, bits 7-11 when non-zero, and
//                            the device
//   1d OP...                 iif ifi ida sci scf, or "1d none"
//   unknown CC               the spare instruction code
//   reserved                 a table word with no value
//
// After the bar: the spelling name, then the opcode-class symbols and any i
// the source wrote, in source order, then "1d" when a PDP-1D mnemonic was
// used.  Task A6's checker compares this dump against a hand-written file.
void
optDumpDecoded(FILE *fP, OptTableP tableP)
{
int i;
OptWordP entryP;

    for( i = 0; i < tableP->count; ++i )
    {
        entryP = tableP->entriesPP[i];

        fprintf(fP, "%02o%04o ", entryP->bank, entryP->addr);

        if( entryP->flags & OPTF_RESERVED )
        {
            fprintf(fP, "------ %-7s reserved", kindName(entryP->kind));
        }
        else
        {
            fprintf(fP, "%06o %-7s ", entryP->value, kindName(entryP->kind));
            printGroup(fP, &entryP->decode);
        }

        fprintf(fP, " | %s", optSpellingName(entryP->decode.spelling));

        if( !(entryP->flags & OPTF_RESERVED) )
        {
            printSpellingNames(fP, entryP->exprP);
        }

        if( entryP->decode.spelled1D )
        {
            fprintf(fP, " 1d");
        }

        fprintf(fP, "\n");
    }
}

// Is a micro-op present in a set of operate-group bits?  The flag field is
// one slot: a non-zero field with bit 010 clear is clf, with it set is stf.
// Returns 1 if present, 0 if not.
int
optMicroOpPresent(unsigned int microBits, OptMicroId id)
{
    switch( id )
    {
    case OPTMO_CLF:
        return( ((microBits & OPTM_FLAGFIELD) != 0) && ((microBits & OPTM_STF) == 0) );

    case OPTMO_STF:
        return( (microBits & OPTM_STF) != 0 );

    default:
        return( (microBits & optMicroOps[id].bits) != 0 );
    }
}

// List the micro-ops present in microBits in hardware order: by phase, then
// by the order column, then by table position for rows that tie.  An
// insertion sort; there are at most eleven rows.
// Returns how many pointers were stored in opsPP, at most max.
int
optOperateOrder(unsigned int microBits, const OptMicroOp **opsPP, int max)
{
int i;
int j;
int n;
const OptMicroOp *opP;

    n = 0;

    for( i = 0; (i < OPTMO_COUNT) && (n < max); ++i )
    {
        if( !optMicroOpPresent(microBits, (OptMicroId)i) )
        {
            continue;
        }

        opP = &optMicroOps[i];

        // Slide the new row back past every row that comes later than it.
        for( j = n; j > 0; --j )
        {
            if( (opsPP[j - 1]->phase < opP->phase) ||
                ((opsPP[j - 1]->phase == opP->phase) && (opsPP[j - 1]->order <= opP->order)) )
            {
                break;
            }

            opsPP[j] = opsPP[j - 1];
        }

        opsPP[j] = opP;
        ++n;
    }

    return(n);
}

// Apply an operate word's register micro-ops to AC and IO in phase order.
// This is the sanity anchor for the phase table: cla cma in one word gives
// all ones from any AC, and cma then cla as two calls gives zero.  lat ORs
// in tw and lap ORs in pc; the caller supplies both.  lia and lai together
// are the exchange lsw/swp, and either alone copies the register as it
// stands after the clears and complements of the same word.  Flag
// operations and hlt change no register.
void
optSimulateOperate(unsigned int microBits, int ac, int io, int tw, int pc, int *acP, int *ioP)
{
const OptMicroOp *opsP[OPTMO_COUNT];
int n;
int i;
int temp;
int swap;

    n = optOperateOrder(microBits, opsP, OPTMO_COUNT);
    swap = ((microBits & OPTM_LIA) && (microBits & OPTM_LAI));

    for( i = 0; i < n; ++i )
    {
        switch( opsP[i]->id )
        {
        case OPTMO_CLA:
            ac = 0;
            break;

        case OPTMO_CLI:
            io = 0;
            break;

        case OPTMO_LAT:
            ac = (ac | tw);
            break;

        case OPTMO_LAP:
            ac = (ac | pc);
            break;

        case OPTMO_CMA:
            ac = (~ac);
            break;

        case OPTMO_CMI:
            io = (~io);
            break;

        case OPTMO_LIA:
            if( swap )
            {
                // The exchange: both transfers at once; lai is then a no-op below.
                temp = ac;
                ac = io;
                io = temp;
            }
            else
            {
                io = ac;
            }
            break;

        case OPTMO_LAI:
            if( !swap )
            {
                ac = io;
            }
            break;

        case OPTMO_CLF:
        case OPTMO_STF:
        case OPTMO_HLT:
        default:
            break;
        }
    }

    *acP = (ac & WRDMASK);
    *ioP = (io & WRDMASK);
}

// The decoder's self-check (-O=check).  Two kinds of check: the phase-table
// anchors, which run the simulator over the classic cases, and a set of
// decodes whose expected fields were worked out by hand from permsyms.def
// and the handbook.  One line per check goes to fP, then a summary.
// Returns 1 if every check passed, 0 if any failed.
int
optDecoderSelfCheck(FILE *fP)
{
int passed;
int total;
int ac;
int io;
int n;
const OptMicroOp *opsP[OPTMO_COUNT];
OptDecode d;

    passed = 0;
    total = 0;

    // --- phase table anchors ---

    // cla cma in one word, 761200: the clear is applied before the
    // complement, so the result is all ones whatever AC held.
    optDecodeValue(0761200, &d);
    checkResult(fP, "761200 is operate cla cma", (d.group == OPTG_OPERATE) && (d.microBits == (OPTM_CLA | OPTM_CMA)),
        &passed, &total);
    n = optOperateOrder(d.microBits, opsP, OPTMO_COUNT);
    checkResult(fP, "761200 applies cla then cma", (n == 2) && (opsP[0]->id == OPTMO_CLA) && (opsP[1]->id == OPTMO_CMA),
        &passed, &total);
    optSimulateOperate(d.microBits, 0, 0, 0, 0, &ac, &io);
    checkResult(fP, "761200 from AC=0 gives 777777", (ac == 0777777), &passed, &total);
    optSimulateOperate(d.microBits, 0123456, 0, 0, 0, &ac, &io);
    checkResult(fP, "761200 from AC=123456 gives 777777", (ac == 0777777), &passed, &total);

    // cma then cla as two words: complement first, then clear, so zero.
    optSimulateOperate(OPTM_CMA, 0, 0, 0, 0, &ac, &io);
    checkResult(fP, "761000 (cma) from AC=0 gives 777777", (ac == 0777777), &passed, &total);
    optSimulateOperate(OPTM_CLA, ac, 0, 0, 0, &ac, &io);
    checkResult(fP, "then 760200 (cla) gives 0", (ac == 0), &passed, &total);

    // The handbook's own example, page 21: opr 3200 clears AC, ORs in the
    // test word, complements.  With TW = 123 that is the complement of 123.
    optSimulateOperate(03200, 0777777, 0, 0123, 0, &ac, &io);
    checkResult(fP, "763200 (opr 3200) with TW=123 gives 777654", (ac == 0777654), &passed, &total);

    // lat as permsyms.def spells it, 762200, loads exactly the test word.
    optSimulateOperate(02200, 0777777, 0, 0123, 0, &ac, &io);
    checkResult(fP, "762200 (lat) with TW=123 gives 123", (ac == 0123), &passed, &total);

    // cli cla clears both registers.
    optSimulateOperate((OPTM_CLI | OPTM_CLA), 5, 7, 0, 0, &ac, &io);
    checkResult(fP, "765200 (cli cla) clears AC and IO", (ac == 0) && (io == 0), &passed, &total);

    // lsw/swp is an exchange.
    optSimulateOperate(060, 1, 2, 0, 0, &ac, &io);
    checkResult(fP, "760060 (swp) exchanges AC and IO", (ac == 2) && (io == 1), &passed, &total);

    // The PDP-1D transfers complete after everything else in the word
    // (pdp1.c: armed at TP8, done in the next fetch), so they see the
    // cleared and complemented registers.
    optSimulateOperate((OPTM_CLA | OPTM_LIA), 0777, 0123, 0, 0, &ac, &io);
    checkResult(fP, "760220 (cla lia) gives AC=0 IO=0: lia copies the cleared AC", (ac == 0) && (io == 0), &passed, &total);
    optSimulateOperate((OPTM_CMI | OPTM_LAI), 0777, 0123, 0, 0, &ac, &io);
    checkResult(fP, "770040 (cmi lai) gives AC=777654: lai copies the complemented IO", (ac == 0777654) && (io == 0777654),
        &passed, &total);
    optSimulateOperate((OPTM_CMA | OPTM_LAI), 0777, 0123, 0, 0, &ac, &io);
    checkResult(fP, "761040 (cma lai) gives AC=123: the complement is overwritten by the transfer", (ac == 0123),
        &passed, &total);
    optSimulateOperate((OPTM_CLI | OPTM_LIA | OPTM_LAI), 0777, 0123, 0, 0, &ac, &io);
    checkResult(fP, "764060 (cli swp) gives AC=0 IO=777: the exchange sees the cleared IO", (ac == 0) && (io == 0777),
        &passed, &total);
    optSimulateOperate((OPTM_CLI | OPTM_CMI), 0, 0123, 0, 0, &ac, &io);
    checkResult(fP, "774000 (cli cmi) gives IO=777777: the clear comes before the complement", (io == 0777777),
        &passed, &total);

    // --- hand-decoded words ---

    optDecodeValue(0210004, &d);
    checkResult(fP, "210004 is memref lac i 0004",
        (d.group == OPTG_MEMREF) && !strcmp(d.mnemonicP, "lac") && d.indirect && d.memIndirect && (d.address == 04),
        &passed, &total);

    optDecodeValue(0170100, &d);
    checkResult(fP, "170100 is memref jda 0100, not indirect",
        (d.group == OPTG_MEMREF) && !strcmp(d.mnemonicP, "jda") && d.indirect && !d.memIndirect && (d.address == 0100),
        &passed, &total);

    optDecodeValue(0160000, &d);
    checkResult(fP, "160000 is memref cal", (d.group == OPTG_MEMREF) && !strcmp(d.mnemonicP, "cal") && !d.memIndirect,
        &passed, &total);

    optDecodeValue(0650100, &d);
    checkResult(fP, "650100 is skip sza i (do not skip on zero AC)",
        (d.group == OPTG_SKIP) && (d.skipConds == OPTC_SZA) && d.skipInverted && !d.skipSwitch && !d.skipFlag,
        &passed, &total);

    optDecodeValue(0640500, &d);
    checkResult(fP, "640500 is skip sma sza",
        (d.group == OPTG_SKIP) && (d.skipConds == (OPTC_SMA | OPTC_SZA)) && !d.skipInverted, &passed, &total);

    optDecodeValue(0640067, &d);
    checkResult(fP, "640067 is skip szs6 szf7",
        (d.group == OPTG_SKIP) && !d.skipConds && (d.skipSwitch == 6) && (d.skipFlag == 7), &passed, &total);

    optDecodeValue(0654000, &d);
    checkResult(fP, "654000 (szi) is skip sni i (skip if IO is zero), PDP-1D",
        (d.group == OPTG_SKIP) && (d.skipConds == OPTC_SNI) && d.skipInverted && !d.skipSwitch && !d.skipFlag &&
        d.uses1D, &passed, &total);

    optDecodeValue(0661007, &d);
    checkResult(fP, "661007 is shift ral 3",
        (d.group == OPTG_SHIFT) && !strcmp(d.mnemonicP, "ral") && (d.shiftCount == 3) && !d.shiftRight && !d.shiftArith &&
        (d.shiftRegs == 1), &passed, &total);

    optDecodeValue(0677777, &d);
    checkResult(fP, "677777 is shift scr 9",
        (d.group == OPTG_SHIFT) && !strcmp(d.mnemonicP, "scr") && (d.shiftCount == 9) && d.shiftRight && d.shiftArith &&
        (d.shiftRegs == 3), &passed, &total);

    optDecodeValue(0660000, &d);
    checkResult(fP, "660000 is shift sft 0", (d.group == OPTG_SHIFT) && !strcmp(d.mnemonicP, "sft") && (d.shiftCount == 0),
        &passed, &total);

    optDecodeValue(0710020, &d);
    checkResult(fP, "710020 is law i 0020", (d.group == OPTG_LAW) && d.indirect && (d.address == 020), &passed, &total);

    optDecodeValue(0730003, &d);
    checkResult(fP, "730003 is iot tyo wait dev 03",
        (d.group == OPTG_IOT) && !strcmp(d.mnemonicP, "tyo") && d.iotWait && !d.iotComplete && !d.iotSub &&
        (d.iotDevice == 03), &passed, &total);

    optDecodeValue(0724074, &d);
    checkResult(fP, "724074 is iot eem cpl dev 74",
        (d.group == OPTG_IOT) && !strcmp(d.mnemonicP, "eem") && !d.iotWait && d.iotComplete && (d.iotDevice == 074),
        &passed, &total);

    optDecodeValue(0722007, &d);
    checkResult(fP, "722007 is iot sdb sub 20 dev 07",
        (d.group == OPTG_IOT) && !strcmp(d.mnemonicP, "sdb") && (d.iotSub == 020) && (d.iotDevice == 07), &passed, &total);

    optDecodeValue(0720123, &d);
    checkResult(fP, "720123 is an unnamed iot", (d.group == OPTG_IOT) && !strcmp(d.mnemonicP, "iot"), &passed, &total);

    optDecodeValue(0770000, &d);
    checkResult(fP, "770000 is operate cmi, PDP-1D",
        (d.group == OPTG_OPERATE) && (d.microBits == OPTM_CMI) && d.uses1D, &passed, &total);

    optDecodeValue(0760017, &d);
    checkResult(fP, "760017 is operate stf 7",
        (d.group == OPTG_OPERATE) && (d.flagOp == OPTFLAG_STF) && (d.flagNum == 7) && !d.uses1D, &passed, &total);

    optDecodeValue(0760003, &d);
    checkResult(fP, "760003 is operate clf 3",
        (d.group == OPTG_OPERATE) && (d.flagOp == OPTFLAG_CLF) && (d.flagNum == 3), &passed, &total);

    optDecodeValue(0760000, &d);
    checkResult(fP, "760000 is operate nop",
        (d.group == OPTG_OPERATE) && !d.microBits && (d.flagOp == OPTFLAG_NONE), &passed, &total);

    optDecodeValue(0744000, &d);
    checkResult(fP, "744000 is 1d iif", (d.group == OPTG_1D) && (d.specialBits == OPTX_IIF) && d.uses1D, &passed, &total);

    optDecodeValue(0000000, &d);
    checkResult(fP, "000000 is unknown 00", (d.group == OPTG_UNKNOWN) && (d.opcode == 0), &passed, &total);

    // Code 12 held jfd while permsyms.def defined one there; it is spare now,
    // and an address field in the word must not talk the decoder out of that.
    optDecodeValue(0120100, &d);
    checkResult(fP, "120100 is unknown 12", (d.group == OPTG_UNKNOWN) && (d.opcode == 012), &passed, &total);

    optDecodeValue(0140000, &d);
    checkResult(fP, "140000 is unknown 14", (d.group == OPTG_UNKNOWN) && (d.opcode == 014), &passed, &total);

    optDecodeValue(0360000, &d);
    checkResult(fP, "360000 is unknown 36", (d.group == OPTG_UNKNOWN) && (d.opcode == 036), &passed, &total);

    fprintf(fP, "decoder self-check: %d of %d checks passed\n", passed, total);
    return( passed == total );
}

// Count the one bits in a value.
// Returns the count.
static int
bitCount(unsigned int bits)
{
int n;

    for( n = 0; bits; bits >>= 1 )
    {
        n += (bits & 1);
    }

    return(n);
}

// Find the permanent opcode-type symbol with exactly this value, walking the
// permanent symbol tree.  Used to name iot words.
// Returns the symbol's name, or NILP if no opcode symbol has the value.
static const char *
findPermOpcodeName(SymNodeP symP, int value)
{
const char *nameP;

    if( !symP )
    {
        return(NILP);
    }

    if( ((symP->flags & SYM_MASK) == SYM_OPCODE) && (symP->value == value) )
    {
        return(symP->name);
    }

    if( (nameP = findPermOpcodeName(symP->leftP, value)) )
    {
        return(nameP);
    }

    return( findPermOpcodeName(symP->rightP, value) );
}

// Walk one word's expression tree counting what it is made of.  The
// separator (the space between "lac" and "x") and parentheses are
// transparent; every other operator is arithmetic, and inArith says whether
// the walk is currently inside one.  A [..] is counted and not entered: what
// is inside it spells the pool word, not this one.
static void
scanShape(PNodeP nodeP, ShapeP shapeP, int inArith)
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
            scanShape(nodeP->leftP, shapeP, inArith);
            scanShape(nodeP->rightP, shapeP, inArith);
        }
        else
        {
            if( !inArith )
            {
                ++shapeP->arithCount;
            }

            scanShape(nodeP->leftP, shapeP, 1);
            scanShape(nodeP->rightP, shapeP, 1);
        }
        break;

    case UNOP:
        if( nodeP->value.ival == PARENS )
        {
            scanShape(nodeP->rightP, shapeP, inArith);
        }
        else
        {
            if( !inArith )
            {
                ++shapeP->arithCount;
            }

            scanShape(nodeP->rightP, shapeP, 1);
        }
        break;

    case LAW:
        ++shapeP->lawCount;
        noteOpSym(shapeP, nodeP->value.symP);
        shapeP->opInArith |= inArith;
        break;

    case OPADDR:
        ++shapeP->opaddrCount;
        noteOpSym(shapeP, nodeP->value.symP);
        shapeP->opInArith |= inArith;
        break;

    case OPCODE:
    case OPORABLE:
        ++shapeP->opcodeCount;
        noteOpSym(shapeP, nodeP->value.symP);
        shapeP->opInArith |= inArith;

        if( nodeP->value.symP && (nodeP->value.symP->flags & SYMF_1DOP) )
        {
            ++shapeP->oneDCount;
        }
        break;

    case IMOD:
        ++shapeP->imodCount;
        break;

    case VALUESPEC:
        ++shapeP->modifierCount;
        break;

    case INTEGER:
    case CHAR:
    case FLEXO:
    case LITCHAR:
    case DOT:
        ++shapeP->numberCount;
        break;

    case ADDR:
    case LCLADDR:
    case BREF:
    case WILDREF:
        ++shapeP->addrSymCount;
        break;

    case CONSTANT:
        ++shapeP->constRefCount;
        break;

    default:
        ++shapeP->otherCount;
        break;
    }
}

// Remember the first opcode-class symbol the scanner meets.
static void
noteOpSym(ShapeP shapeP, SymNodeP symP)
{
    if( !shapeP->firstOpSymP )
    {
        shapeP->firstOpSymP = symP;
    }
}

// Print the group and field text of one decoded word, in the format
// described at optDumpDecoded().
static void
printGroup(FILE *fP, OptDecodeP decodeP)
{
int i;
int any;

    switch( decodeP->group )
    {
    case OPTG_MEMREF:
        fprintf(fP, "memref %s%s %04o", decodeP->mnemonicP, (decodeP->memIndirect)?" i":"", decodeP->address);
        break;

    case OPTG_LAW:
        fprintf(fP, "law%s %04o", (decodeP->indirect)?" i":"", decodeP->address);
        break;

    case OPTG_SKIP:
        fprintf(fP, "skip");
        any = 0;

        for( i = 0; i < (int)(sizeof(skipCondNames) / sizeof(skipCondNames[0])); ++i )
        {
            if( decodeP->skipConds & skipCondNames[i].bit )
            {
                fprintf(fP, " %s", skipCondNames[i].nameP);
                any = 1;
            }
        }

        if( decodeP->skipSwitch )
        {
            fprintf(fP, " szs%o", decodeP->skipSwitch);
            any = 1;
        }

        if( decodeP->skipFlag )
        {
            fprintf(fP, " szf%o", decodeP->skipFlag);
            any = 1;
        }

        if( !any )
        {
            fprintf(fP, " skp");
        }

        if( decodeP->skipInverted )
        {
            fprintf(fP, " i");
        }
        break;

    case OPTG_SHIFT:
        fprintf(fP, "shift %s %d", decodeP->mnemonicP, decodeP->shiftCount);
        break;

    case OPTG_OPERATE:
        fprintf(fP, "operate");

        if( !decodeP->microBits )
        {
            fprintf(fP, " nop");
            break;
        }

        for( i = 0; i < (int)(sizeof(operateDumpOrder) / sizeof(operateDumpOrder[0])); ++i )
        {
            if( optMicroOpPresent(decodeP->microBits, operateDumpOrder[i]) )
            {
                fprintf(fP, " %s", optMicroOps[operateDumpOrder[i]].nameP);
            }
        }

        if( decodeP->flagOp == OPTFLAG_CLF )
        {
            fprintf(fP, " clf%o", decodeP->flagNum);
        }
        else if( decodeP->flagOp == OPTFLAG_STF )
        {
            fprintf(fP, " stf%o", decodeP->flagNum);
        }
        break;

    case OPTG_IOT:
        fprintf(fP, "iot %s", decodeP->mnemonicP);

        if( decodeP->iotWait )
        {
            fprintf(fP, " wait");
        }

        if( decodeP->iotComplete )
        {
            fprintf(fP, " cpl");
        }

        if( decodeP->iotSub )
        {
            fprintf(fP, " sub %02o", decodeP->iotSub);
        }

        fprintf(fP, " dev %02o", decodeP->iotDevice);
        break;

    case OPTG_1D:
        fprintf(fP, "1d");
        any = 0;

        for( i = 0; i < (int)(sizeof(specialNames) / sizeof(specialNames[0])); ++i )
        {
            if( decodeP->specialBits & specialNames[i].bit )
            {
                fprintf(fP, " %s", specialNames[i].nameP);
                any = 1;
            }
        }

        if( !any )
        {
            fprintf(fP, " none");
        }
        break;

    case OPTG_UNKNOWN:
    default:
        fprintf(fP, "unknown %02o", decodeP->opcode);
        break;
    }
}

// Print the opcode-class symbols and any i in a word's expression, in
// source order (the tree's in-order walk), each preceded by a space.  This is
// the dump's record of what the source actually wrote, next to what the bits
// decode to.
static void
printSpellingNames(FILE *fP, PNodeP nodeP)
{
    if( !nodeP )
    {
        return;
    }

    switch( nodeP->type )
    {
    case BINOP:
        printSpellingNames(fP, nodeP->leftP);
        printSpellingNames(fP, nodeP->rightP);
        break;

    case UNOP:
        printSpellingNames(fP, nodeP->rightP);
        break;

    case LAW:
    case OPADDR:
    case OPCODE:
    case OPORABLE:
        if( nodeP->value.symP )
        {
            fprintf(fP, " %s", nodeP->value.symP->name);
        }
        break;

    case IMOD:
        fprintf(fP, " i");
        break;

    default:
        break;
    }
}

// Record and print one self-check result.
static void
checkResult(FILE *fP, const char *whatP, int passed, int *passedP, int *totalP)
{
    ++*totalP;

    if( passed )
    {
        ++*passedP;
        fprintf(fP, "  ok   %s\n", whatP);
    }
    else
    {
        fprintf(fP, "  FAIL %s\n", whatP);
    }
}

// ===========================================================================
// Task A3: reference edges, the reverse index, pointer following, the word
// flags, the code/data classification and the conflict set.
//
// The analysis makes five passes over the decoded table:
//
//   1. Direct edges.  Every word with an expression is walked once.  A word
//      that is a memory reference instruction (decoded as one AND spelled
//      with exactly one address-taking mnemonic) gets an edge for its
//      address field, to the decoded address, in the role the mnemonic
//      implies; the symbol in that field, if the field is a single symbol,
//      rides on the edge.  Every other symbol leaf in the word, and every
//      symbol leaf of a word that is not such an instruction (a data word, a
//      law, a table or var initializer, a constant-pool word) becomes a
//      "taken" edge to the symbol's own word.  jda and cal add the edges the
//      hardware makes without being told.
//   2. Direct flags.  written, patched, taken and the rest are derived from
//      the in edges.  Then the placeholders are marked: an edge whose source
//      word is patched and whose field was not a symbol ("rtn, jmp 0", "dac
//      0") records the value the word holds before it is patched, which is
//      never what runs; such an edge is kept for the dump but is not
//      followed and does not classify its target.  The flags are derived
//      again without them, so that pass 3 can ask whether a pointer word is
//      ever written.
//   3. Indirect edges.  An edge made by an instruction with the i bit set
//      went to the pointer word.  If that word is data, is never written and
//      holds a resolved address, a second edge goes to the address it holds,
//      marked via-pointer.  Otherwise the target is unknown, and every word
//      in the referencing bank whose address is taken is marked as possibly
//      reached in the edge's role.
//   4. Final flags, from every edge including the via-pointer ones.
//   5. Classification and conflicts, by the rules of study section 5.3.
// ===========================================================================

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

#define CAL_WRITE_ADDR  0100    // cal deposits AC here (handbook page 18: cal is jda 100)
#define CAL_JUMP_ADDR   0101    // and continues here

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
static const char *firstLabelName(OptWordP entryP);

// Build the references of a decoded table: the five passes described above.
// Unresolved references are reported on stderr, counted, and skipped.
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
// source and no symbol.  Either list is "-" when empty.  The check script in
// Tests/References compares this dump against a hand-written file.
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
// (text, a reserved table word, an uninitialized variable) references
// nothing.
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
        // Not an instruction with an address field: every symbol in the
        // word is merely being used as a value.
        makeLeafEdges(tableP, entryP, entryP->exprP, NILP);
    }
}

// Is this word a memory reference instruction with an address field?  It
// must decode as one AND be spelled with exactly one address-taking mnemonic
// and nothing else opcode-like: "lac x", "dac i p", "lac [0]", "cal", but
// not "jmp+x", "jmp x cla" or a bare 0200000.  The spelling is the
// evidence: a data word whose bits happen to decode as lac does not read
// anything.
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

// Find the address field's symbol of an instruction word: the one symbol
// leaf (ADDR, LCLADDR, BREF or a [..] CONSTANT) that is not inside
// arithmetic, provided it is the only such leaf.  "lac x", "lac i x",
// "lac (x)" and "lac [x]" have one; "lac x+1", "lac 100" and "lac x y" have
// none.
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

// Count the symbol leaves of an expression that are not inside arithmetic,
// remembering the last one.  The separator and parentheses are transparent;
// every other operator makes its operands arithmetic.  A [..] is a leaf, not
// entered.
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

// Walk an expression and make a taken edge for every symbol leaf other than
// the address field's own (fieldP, NILP when there is none).  The field's
// edge was made by makeInstructionEdges().
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

// Make the edges of a memory reference instruction's address field.  The
// target is the decoded address in the word's own bank, whatever the
// source wrote to get there; the symbol rides along when the field was a
// single symbol, otherwise the edge is implicit.  jda writes the word it
// names and enters the one after; cal writes 100 and enters 101 and ignores
// its field altogether, so any symbol in a cal word is only taken.
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

// Where does a symbol leaf point?  An ADDR or LCLADDR names a word in the
// referencing word's own bank; a BREF names one in the bank it was
// qualified with; a CONSTANT names its pool word, which is always in the
// referencing bank.  A symbol that never resolved (which the assembler
// would already have rejected) or a WILDREF left unresolved is reported and
// refused.
// Returns 1 with the bank and address stored, 0 if the leaf cannot be placed.
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

// Add an edge from a word to every word at a bank and address.  An address
// with nothing emitted at it gets one edge flagged OPTEF_NOWORD, kept on the
// source's list so the dump and the report can show it; an overlaid address
// gets one edge per entry there, so each entry's reverse index is complete.
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

// Make one edge and link it into the source's out list, the target's in
// list and the statistics.
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

        // The in counts are derived from the list by deriveFlags(), which
        // knows which edges count and in what role.
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

// Free one word's out list.  The in lists share these edges and are not
// walked.
static void
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

// Passes 2 and 4: derive the in counts and the edge-implied flags of every
// word from its in edges.  Recomputed from scratch each time, so the pass
// can run before and after the placeholders are marked and the via-pointer
// edges exist; the conservative "maybe" marks and the start flag are not
// edge-implied and are left alone.  An indirect edge lands on the pointer
// word, which the hardware READS to find the real target, so it counts as
// a read whatever its role; the role reaches the real target through the
// via-pointer edge or the conservative marks.  A placeholder edge counts for
// nothing.
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

// Pass 2, second half: mark the placeholder edges.  A patched word's
// address field is replaced before the word runs, so an edge from it that
// no symbol named (the 0 of "rtn, jmp 0", the 0 of a "dac 0" slot) points
// at nothing real; a field that a symbol named ("ld, lac tbl" advanced by
// idx) is real at least once and is kept.  A cascade through a placeholder
// dap or dip (a "dap 0" that is itself patched) is not iterated.
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

// Pass 3: for every indirect edge, follow the pointer word it went to.  A
// pointer that can be followed yields a second edge, in the same role, to
// the address the pointer holds; one that cannot marks the referencing
// bank conservatively.  The via edges are appended to the out lists being
// walked; they carry OPTEF_VIAPOINTER and are skipped when the walk reaches
// them.
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

// Can a pointer word be followed, and where to?  It can when it is data
// (not an expression spelled with a mnemonic, not code by any edge), is
// never directly written, is not overlaid, and holds a value that is an
// address: bits 0 and 1 clear.  Which bank the address is in is decided by
// the pointer's own source when it can be: a bank-qualified reference
// ("[foo:0]", "[foo:*]" once resolved) is a 16-bit pointer into the bank it
// names, the memory.ah convention for cross-bank calls under eem, and bank
// 0 is as explicit as any other.  Without a qualifier, a value with no bank
// bits is a 12-bit pointer and names the referencing instruction's own
// bank, which is what the hardware does with extend mode off; a value with
// bank bits is taken as a 16-bit pointer into that bank if the bank emitted
// anything, and otherwise is more likely a chained-indirect word than a
// pointer and is not followed.  A pointer written only through another
// pointer is not detected here: "never written" means no direct write edge.
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

// Which bank does a pointer word's expression name?  The first
// bank-qualified reference (a BREF leaf) decides; the separator,
// parentheses and arithmetic are looked through, so "[foo:0+1]" names bank
// 0.
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

// Does any direct write edge enter a word?  A via-pointer write is not
// direct, and an indirect write only reads the word on its way through.
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

// Does any write edge other than a patch enter a word?  A dap or dip into
// a word is evidence that the word is an instruction, not data, so the data
// rule of classifyWords() asks this rather than "is it written".
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

// An indirect reference whose pointer could not be followed may reach any
// word whose address is used as a value: mark every taken word of the bank
// as possibly reached in the reference's role, and remember on the table
// that the bank carries such marks.
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

// Pass 5: the start flag, the code/data classification and the conflicts,
// by study section 5.3:
//
//   code: a jump or execute edge comes in, or the word is at the start
//         address, or it is an expression spelled as an instruction (a
//         mnemonic with or without an operand, a law, or a mnemonic with a
//         [..] operand); task A4 refines the spelling case by reachability.
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

            default:
                // OTHER: a shape that argues for neither.
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
            // A pool symbol's name is its fingerprint, which no reader
            // wants; the source wrote [..].
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
static void
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
static void
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
static const char *
firstLabelName(OptWordP entryP)
{
    if( entryP->labelsP && entryP->labelsP->symP && entryP->labelsP->symP->name )
    {
        return( entryP->labelsP->symP->name );
    }

    return("-");
}

// ===========================================================================
// Task A4: basic blocks, control-flow edges and reachability.
//
// The graph is laid over the words task A3 called code, cut into blocks that
// can only be entered at their first word, with one shared UNKNOWN sink for
// every edge whose target this analysis cannot name.  Study section 5.4 has
// the rules; the deviations from it, and the facts behind them, are in the
// comments on the functions that make them.
// ===========================================================================

// The per-bank "a block starts at this address" maps.  Built in full before
// any block is formed, because a via-pointer jump found in one bank can name
// a target in another and the target's bank may already have been walked.
// One byte per address of every bank that emitted a word: 4K per bank, 64K
// in the worst case, released as soon as the blocks exist.
typedef struct
{
    char *marksP[MAXBANK + 1];      // NILP for a bank that emitted nothing
} StartMap, *StartMapP;

// The word flags the control-flow dump prints, in the order it prints them.
static const struct
{
    unsigned int bit;
    const char *nameP;
} flowFlagNames[] =
{
    { OPTF_ENTRY,       "entry" },
    { OPTF_BLOCKSTART,  "blockstart" },
    { OPTF_HASLABEL,    "label" },
    { OPTF_AFTERSKIP,   "afterskip" },
    { OPTF_UNREACHED,   "unreached" },
    { OPTF_XCTONLY,     "xctonly" }
};

static int inGraph(OptWordP entryP);
static OptWordP wordAt(OptTableP tableP, int bank, int addr);
static int optSkipWord(OptWordP entryP);
static OptBlockEnd optTerminator(OptWordP entryP);
static int hasExportedLabel(OptWordP entryP);
static int isSbsEntry(int bank, int addr);
static OptEntryCause entryCause(OptWordP entryP);
static void markFlowWordFlags(OptTableP tableP);
static void countExcludedWords(OptTableP tableP);
static void allocStartMap(OptTableP tableP, StartMapP mapP);
static void freeStartMap(OptTableP tableP, StartMapP mapP);
static void markStart(StartMapP mapP, int bank, int addr);
static void markBlockStarts(OptTableP tableP, StartMapP mapP);
static void markControlTargets(OptTableP tableP, StartMapP mapP, OptWordP entryP);
static void formBlocks(OptTableP tableP, StartMapP mapP);
static OptBlockP newBlock(OptTableP tableP, OptWordP entryP);
static void setBlockEndKinds(OptTableP tableP);
static OptFlowEdgeP addFlowEdge(OptTableP tableP, OptBlockP blockP, OptFlowKind kind);
static void addSinkEdge(OptTableP tableP, OptBlockP blockP, OptFlowKind kind, OptSink sink);
static void addTargetEdge(OptTableP tableP, OptBlockP blockP, OptFlowKind kind, int bank, int addr);
static int viaJumpEdges(OptTableP tableP, OptBlockP blockP, OptFlowKind kind, OptWordP entryP);
static void addTransferEdges(OptTableP tableP, OptBlockP blockP, OptFlowKind kind, OptWordP entryP);
static void buildBlockEdges(OptTableP tableP, OptBlockP blockP);
static void markEntries(OptTableP tableP);
static void walkReachability(OptTableP tableP);
static void markUnreached(OptTableP tableP);
static int blockHasUnreachedWord(OptTableP tableP, OptBlockP blockP);

// Overlay the control-flow graph on a decoded, referenced table.  The order
// of the passes matters: the per-word flags come first because the block
// starts consult none of them but the dump prints all of them; the start maps
// are complete before a single block is formed, so that every in-graph edge
// target is guaranteed to be the FIRST word of some block and an edge can
// never land in the middle of one; the edges need every block to exist, so
// they follow block forming; and reachability needs the edges.
void
optBuildFlow(OptTableP tableP)
{
StartMap map;
OptBlockP blockP;

    if( !tableP )
    {
        return;
    }

    markFlowWordFlags(tableP);
    countExcludedWords(tableP);

    allocStartMap(tableP, &map);
    markBlockStarts(tableP, &map);
    formBlocks(tableP, &map);
    freeStartMap(tableP, &map);

    setBlockEndKinds(tableP);

    for( blockP = tableP->blocksP; blockP; blockP = blockP->nextP )
    {
        buildBlockEdges(tableP, blockP);

        tableP->graphWords += blockP->wordCount;

        if( blockP->wordCount > tableP->largestBlock )
        {
            tableP->largestBlock = blockP->wordCount;
        }
    }

    markEntries(tableP);
    walkReachability(tableP);
    markUnreached(tableP);
}

// Is a word part of the control-flow graph?  Three kinds are left out: a
// reserved table word has no value to decode, an overlaid address holds more
// than one emitted word so which one the machine would fetch is unanswerable
// (study 5.1 calls such addresses opaque), and a word A3 did not classify as
// code is not something the machine is believed to execute.
// Returns 1 when the word is in the graph, 0 when it is not or is NILP.
static int
inGraph(OptWordP entryP)
{
    if( !entryP )
    {
        return(0);
    }

    if( entryP->flags & (OPTF_RESERVED | OPTF_DUPADDR) )
    {
        return(0);
    }

    if( !(entryP->flags & OPTF_CODE) )
    {
        return(0);
    }

    return(1);
}

// Find the first word emitted at a bank and address.
// Returns the word, or NILP when the bank emitted nothing, nothing was
// emitted at that address, or the coordinates are out of range.
static OptWordP
wordAt(OptTableP tableP, int bank, int addr)
{
OptBankP bankP;

    if( (bank < 0) || (bank > MAXBANK) || (addr < 0) || (addr >= BANKSIZE) )
    {
        return(NILP);
    }

    if( !(bankP = tableP->banksP[bank]) )
    {
        return(NILP);
    }

    return( bankP->wordsP[addr] );
}

// Does a word's decode make it a skip-class instruction, one that may leave
// the word after it unexecuted?  The skip group proper is code 64 and 65,
// which covers every condition, the PDP-1D sni and szi, and the reversed
// sense of bit 5.  Three memory reference instructions also skip: isp on the
// incremented word being positive, sad and sas on the comparison.  An iot
// with bit 5 set WAITS for a completion pulse rather than skipping, so the
// in-out group is deliberately not in this set.  No attempt is made to
// notice that skp with no condition bits (640000) can never skip; the extra
// edge costs an unreachable-looking word nothing.
// Returns 1 for a skip-class word, 0 otherwise or for NILP.
static int
optSkipWord(OptWordP entryP)
{
    if( !entryP || (entryP->flags & OPTF_RESERVED) )
    {
        return(0);
    }

    if( entryP->decode.group == OPTG_SKIP )
    {
        return(1);
    }

    if( entryP->decode.group == OPTG_MEMREF )
    {
        switch( entryP->decode.opcode )
        {
        case 046:       // isp
        case 050:       // sad
        case 052:       // sas
            return(1);

        default:
            return(0);
        }
    }

    return(0);
}

// Does a word end a basic block of its own accord, and how?  xct is
// deliberately NOT a terminator: it executes another word in place, and if
// that word is a jump then control does leave.  Treating xct as a
// fall-through can therefore call a word reached that is not, which makes
// the unreached report say LESS than the truth rather than more -- the safe
// direction for a report that is already labeled a hint.  Noted for A5.
// Returns the ending kind, or OPTBE_COUNT when the word is not a terminator.
static OptBlockEnd
optTerminator(OptWordP entryP)
{
    if( !entryP || (entryP->flags & OPTF_RESERVED) )
    {
        return(OPTBE_COUNT);
    }

    if( optSkipWord(entryP) )
    {
        return(OPTBE_SKIP);
    }

    if( entryP->decode.group == OPTG_MEMREF )
    {
        switch( entryP->decode.opcode )
        {
        case 060:       // jmp
            return(OPTBE_JUMP);

        case 062:       // jsp
        case 016:       // cal, and jda when bit 5 is set
            return(OPTBE_CALL);

        default:
            return(OPTBE_COUNT);
        }
    }

    // hlt stops the machine at TP9.  Nothing else in the operate group ends a
    // block; the rest are register operations the next word follows.
    if( (entryP->decode.group == OPTG_OPERATE) && (entryP->decode.microBits & OPTM_HLT) )
    {
        return(OPTBE_HALT);
    }

    return(OPTBE_COUNT);
}

// Does any label on a word name it to other programs?  An exported symbol is
// an address another assembly can jump to, so the word it names has to be
// treated as enterable from outside this program entirely.
// Returns 1 when at least one label is exported, 0 otherwise.
static int
hasExportedLabel(OptWordP entryP)
{
OptLabelP labelP;

    for( labelP = entryP->labelsP; labelP; labelP = labelP->nextP )
    {
        if( labelP->symP && (labelP->symP->flags & SYMF_EXPORTED) )
        {
            return(1);
        }
    }

    return(0);
}

// Is an address a sequence-break handler entry?  Each channel of the Type 20
// system owns four words of bank 0 starting at address 0, so a full sixteen
// channel system uses 0 through 077 -- which is why programs conventionally
// begin at 0100 (project owner, 08-Sep-2026).  Within a frame the hardware
// stores the pre-break AC at 4n+0, the packed PC to return to at 4n+1 and IO
// at 4n+2, then begins EXECUTING at 4n+3, where the program conventionally
// puts a jmp to the real handler (owner, and the pdp1-emulator skill's
// references/sbs.md).  So 4n+3 is an entry: the hardware transfers control
// there and nothing in the program need ever name the address.
// Returns 1 for a handler entry address, 0 otherwise.
static int
isSbsEntry(int bank, int addr)
{
    if( bank != 0 )
    {
        return(0);
    }

    if( (addr < 0) || (addr >= (OPTSBS_CHANNELS * OPTSBS_FRAMESIZE)) )
    {
        return(0);
    }

    return( (addr % OPTSBS_FRAMESIZE) == OPTSBS_ENTRYSLOT );
}

// Why is a word an entry of the reachability walk?  A word can qualify under
// more than one cause; the first that applies is the one reported, so the
// per-cause counts always add up to the number of entries.  Derived rather
// than stored, so the report, the dump and the walk can never disagree.
// Returns the cause, or OPTEN_COUNT when the word is not an entry.
static OptEntryCause
entryCause(OptWordP entryP)
{
    if( entryP->flags & OPTF_START )
    {
        return(OPTEN_START);
    }

    // Study 5.4: a word whose address is used as a value could be reached by
    // an indirect jump or an xct that this analysis cannot follow.
    if( entryP->flags & OPTF_TAKEN )
    {
        return(OPTEN_TAKEN);
    }

    if( hasExportedLabel(entryP) )
    {
        return(OPTEN_EXPORT);
    }

    if( isSbsEntry(entryP->bank, entryP->addr) )
    {
        return(OPTEN_SBS);
    }

    return(OPTEN_COUNT);
}

// Pass 1: clear any flow state left from an earlier run and set the two
// per-word flags that do not depend on the graph.
static void
markFlowWordFlags(OptTableP tableP)
{
int i;
OptWordP entryP;
OptWordP prevP;

    for( i = 0; i < tableP->count; ++i )
    {
        entryP = tableP->entriesPP[i];
        entryP->flags &= ~OPTF_FLOW_MASK;
        entryP->blockP = NILP;

        // P1 (no side entry into a pattern) treats ANY label as a side entry,
        // even one that is only ever taken as a value, so this flag is about
        // the label's existence and says nothing about how it is used.  It is
        // deliberately distinct from being a block start: an interior word of
        // an instruction pattern can carry a data label and still be interior
        // from the machine's point of view.
        if( entryP->labelsP )
        {
            entryP->flags |= OPTF_HASLABEL;
        }

        // P4 asks whether this word might be skipped over.  It looks one
        // address down whatever the classification of what it finds: a data
        // word whose bits happen to spell sza is still a word the machine
        // would skip on if it ever executed it, and being wrong in this
        // direction only suppresses a transform.  An overlaid address counts
        // if ANY of the words emitted there decodes as a skip.  Address 0 has
        // no predecessor; the flag does not wrap to the top of the bank.
        if( entryP->addr > 0 )
        {
            for( prevP = wordAt(tableP, entryP->bank, (entryP->addr - 1)); prevP; prevP = prevP->sameAddrP )
            {
                if( optSkipWord(prevP) )
                {
                    entryP->flags |= OPTF_AFTERSKIP;
                    break;
                }
            }
        }
    }
}

// Count the words the graph leaves out, split by the reason, so the report
// can account for the difference between the table's word count and the
// graph's.  Reserved words are not counted here; the table already counts
// them separately and they have no value to classify.
static void
countExcludedWords(OptTableP tableP)
{
int i;
OptWordP entryP;

    for( i = 0; i < tableP->count; ++i )
    {
        entryP = tableP->entriesPP[i];

        if( inGraph(entryP) || (entryP->flags & OPTF_RESERVED) )
        {
            continue;
        }

        if( entryP->flags & OPTF_DUPADDR )
        {
            ++tableP->overlaidWords;
        }
        else
        {
            ++tableP->notCodeWords;
        }
    }
}

// Allocate a start map for every bank that emitted a word.  Allocation
// failure is fatal, as it is everywhere else in am1.
static void
allocStartMap(OptTableP tableP, StartMapP mapP)
{
int bank;

    for( bank = 0; bank <= MAXBANK; ++bank )
    {
        mapP->marksP[bank] = NILP;

        if( !tableP->banksP[bank] )
        {
            continue;
        }

        if( !(mapP->marksP[bank] = (char *)calloc(BANKSIZE, sizeof(char))) )
        {
            fprintf(stderr, "am1: out of memory building the optimizer control-flow graph\n");
            exit(1);
        }
    }
}

// Release the start maps.
static void
freeStartMap(OptTableP tableP, StartMapP mapP)
{
int bank;

    (void)tableP;

    for( bank = 0; bank <= MAXBANK; ++bank )
    {
        if( mapP->marksP[bank] )
        {
            free(mapP->marksP[bank]);
            mapP->marksP[bank] = NILP;
        }
    }
}

// Mark one address as starting a block.  Coordinates outside the map, or in a
// bank that emitted nothing, are ignored: there is no word there to begin a
// block with, and the edge that named it will find the sink on its own.
static void
markStart(StartMapP mapP, int bank, int addr)
{
    if( (bank < 0) || (bank > MAXBANK) || (addr < 0) || (addr >= BANKSIZE) )
    {
        return;
    }

    if( !mapP->marksP[bank] )
    {
        return;
    }

    mapP->marksP[bank][addr] = 1;
}

// Pass 2: mark every address that begins a block, from the words themselves
// and from the targets their control transfers name.
static void
markBlockStarts(OptTableP tableP, StartMapP mapP)
{
int i;
OptWordP entryP;

    for( i = 0; i < tableP->count; ++i )
    {
        entryP = tableP->entriesPP[i];

        if( !inGraph(entryP) )
        {
            continue;
        }

        // Something outside this analysis' view can land on the start
        // address, on any word A3 saw a jump edge into, and on any word whose
        // address is taken as a value.  An exported label offers the word to
        // another program, and a sequence-break entry is reached by the
        // hardware without anything naming it.  All five have to begin a
        // block, or a transform could rewrite the words around them.
        if( entryP->flags & (OPTF_START | OPTF_JUMPTARGET | OPTF_TAKEN) )
        {
            markStart(mapP, entryP->bank, entryP->addr);
        }
        else if( hasExportedLabel(entryP) || isSbsEntry(entryP->bank, entryP->addr) )
        {
            markStart(mapP, entryP->bank, entryP->addr);
        }

        markControlTargets(tableP, mapP, entryP);
    }
}

// Mark the addresses one in-graph word's control transfers name.  The targets
// are computed here from the DECODED bits, the same way buildBlockEdges()
// computes them, so that every edge that lands in the graph is guaranteed to
// land on the first word of a block.  Deriving them from A3's edges instead
// would not be enough: A3 gives a word an address-field edge only when the
// source SPELLED it as a memory reference, so a code word whose bits decode
// as jda but which the source wrote some other way would have had no edge and
// its target would have gone unmarked.
static void
markControlTargets(OptTableP tableP, StartMapP mapP, OptWordP entryP)
{
OptBlockEnd end;
OptEdgeP edgeP;
int addr;

    (void)tableP;

    end = optTerminator(entryP);
    addr = entryP->decode.address;

    if( end == OPTBE_COUNT )
    {
        return;
    }

    // Every terminator's own next word begins a block: the fall-through of a
    // skip, the return of a call, the resume of a halt, and, after a jmp, a
    // word nothing falls into but which cannot belong to this block either.
    markStart(mapP, entryP->bank, ((entryP->addr + 1) & ADDRMASK));

    if( end == OPTBE_SKIP )
    {
        markStart(mapP, entryP->bank, ((entryP->addr + 2) & ADDRMASK));
        return;
    }

    if( (end != OPTBE_JUMP) && (end != OPTBE_CALL) )
    {
        return;
    }

    // A patched word's address field is a run-time value and DEBREAK returns
    // to whatever it interrupted.  Both reach the sink and mark nothing.
    if( entryP->flags & OPTF_PATCHED )
    {
        return;
    }

    if( entryP->decode.memIndirect )
    {
        if( (entryP->bank == 0) && (addr == OPTDEBREAK_ADDR) )
        {
            return;
        }

        // A3 followed the pointer for us: its via-pointer jump edges are
        // where control actually lands.
        for( edgeP = entryP->outP; edgeP; edgeP = edgeP->nextOutP )
        {
            if( (edgeP->role == OPTR_JUMP) && (edgeP->flags & OPTEF_VIAPOINTER) )
            {
                markStart(mapP, edgeP->toBank, edgeP->toAddr);
            }
        }

        return;
    }

    if( entryP->decode.opcode == 016 )
    {
        // jda deposits AC in the word it names and enters the word after it;
        // cal is jda 0100, so it ignores its address field and enters 0101 of
        // its own bank.
        if( entryP->decode.indirect )
        {
            markStart(mapP, entryP->bank, ((addr + 1) & ADDRMASK));
        }
        else
        {
            markStart(mapP, entryP->bank, CAL_JUMP_ADDR);
        }

        return;
    }

    markStart(mapP, entryP->bank, addr);
}

// Pass 3: cut the in-graph words into blocks, walking each bank in address
// order.  Walking in address order rather than emission order is what makes
// "the word before another block's start ends a block" free: a block simply
// stops when the next address begins one.
static void
formBlocks(OptTableP tableP, StartMapP mapP)
{
int bank;
int addr;
OptWordP entryP;
OptWordP prevP;
OptBlockP blockP;

    for( bank = 0; bank <= MAXBANK; ++bank )
    {
        if( !mapP->marksP[bank] )
        {
            continue;
        }

        blockP = NILP;
        prevP = NILP;

        for( addr = 0; addr < BANKSIZE; ++addr )
        {
            entryP = wordAt(tableP, bank, addr);

            if( !inGraph(entryP) )
            {
                // A gap, a data word or an overlaid address: whatever comes
                // after it cannot continue the block that was open.
                blockP = NILP;
                prevP = NILP;
                continue;
            }

            // A block begins here when nothing is open, when the maps say an
            // address is entered from somewhere, or when the previous word
            // ended a block of its own accord.
            if( !blockP || mapP->marksP[bank][addr] || (optTerminator(prevP) != OPTBE_COUNT) )
            {
                blockP = newBlock(tableP, entryP);
            }
            else
            {
                blockP->endAddr = addr;
                blockP->lastP = entryP;
                ++blockP->wordCount;
            }

            entryP->blockP = blockP;
            prevP = entryP;
        }
    }
}

// Start a new one-word block at a word and append it to the table's list.
// Blocks are numbered from 1 in the order they are created, which is bank
// then address order, so the id ordering and the address ordering agree.
// Returns the new block; allocation failure is fatal.
static OptBlockP
newBlock(OptTableP tableP, OptWordP entryP)
{
OptBlockP blockP;

    if( !(blockP = (OptBlockP)calloc(1, sizeof(OptBlock))) )
    {
        fprintf(stderr, "am1: out of memory building the optimizer control-flow graph\n");
        exit(1);
    }

    blockP->id = ++tableP->blockCount;
    blockP->bank = entryP->bank;
    blockP->startAddr = entryP->addr;
    blockP->endAddr = entryP->addr;
    blockP->wordCount = 1;
    blockP->firstP = entryP;
    blockP->lastP = entryP;
    blockP->endKind = OPTBE_GAP;

    entryP->flags |= OPTF_BLOCKSTART;

    if( tableP->blocksTailP )
    {
        tableP->blocksTailP->nextP = blockP;
    }
    else
    {
        tableP->blocksP = blockP;
    }

    tableP->blocksTailP = blockP;
    return(blockP);
}

// Pass 4: decide how each block ends.  A block that ends on a terminator is
// named by the terminator; one that simply ran into something is named by
// what it ran into, which is what tells a reader whether the block flows on
// into the next one or stops because there is nothing more to execute.
static void
setBlockEndKinds(OptTableP tableP)
{
OptBlockP blockP;
OptBlockEnd end;
OptWordP nextP;

    for( blockP = tableP->blocksP; blockP; blockP = blockP->nextP )
    {
        if( (end = optTerminator(blockP->lastP)) != OPTBE_COUNT )
        {
            blockP->endKind = end;
            continue;
        }

        if( blockP->endAddr >= (BANKSIZE - 1) )
        {
            blockP->endKind = OPTBE_GAP;
            continue;
        }

        nextP = wordAt(tableP, blockP->bank, (blockP->endAddr + 1));

        if( !nextP )
        {
            blockP->endKind = OPTBE_GAP;
        }
        else if( !inGraph(nextP) )
        {
            blockP->endKind = OPTBE_NOTCODE;
        }
        else
        {
            blockP->endKind = OPTBE_BOUNDARY;
        }
    }
}

// Append a successor edge to a block and count it on the table.
// Returns the new edge, with its target still unset; allocation failure is
// fatal.
static OptFlowEdgeP
addFlowEdge(OptTableP tableP, OptBlockP blockP, OptFlowKind kind)
{
OptFlowEdgeP edgeP;

    if( !(edgeP = (OptFlowEdgeP)calloc(1, sizeof(OptFlowEdge))) )
    {
        fprintf(stderr, "am1: out of memory building the optimizer control-flow graph\n");
        exit(1);
    }

    edgeP->kind = kind;
    edgeP->sink = OPTSK_NONE;

    if( blockP->succTailP )
    {
        blockP->succTailP->nextP = edgeP;
    }
    else
    {
        blockP->succP = edgeP;
    }

    blockP->succTailP = edgeP;
    ++blockP->succCount;
    ++tableP->flowEdgeCount;
    ++tableP->flowKindCounts[kind];

    return(edgeP);
}

// Append an edge to the shared UNKNOWN sink, with the reason the analysis
// lost the thread.  A sink edge carries no reachability: it says the target
// is unknown, not that control stopped.
static void
addSinkEdge(OptTableP tableP, OptBlockP blockP, OptFlowKind kind, OptSink sink)
{
OptFlowEdgeP edgeP;

    edgeP = addFlowEdge(tableP, blockP, kind);
    edgeP->sink = sink;
    ++tableP->sinkEdgeCount;
    ++tableP->sinkCounts[sink];
}

// Append an edge to whatever lies at a bank and address, which is a block
// when the graph holds a word there and the sink when it does not.  Every
// in-graph target reached here is the first word of its block, because
// markBlockStarts() marked it before any block was formed.
static void
addTargetEdge(OptTableP tableP, OptBlockP blockP, OptFlowKind kind, int bank, int addr)
{
OptWordP targetP;
OptFlowEdgeP edgeP;

    if( !(targetP = wordAt(tableP, bank, addr)) )
    {
        addSinkEdge(tableP, blockP, kind, OPTSK_NOWORD);
        return;
    }

    if( targetP->flags & OPTF_DUPADDR )
    {
        addSinkEdge(tableP, blockP, kind, OPTSK_OVERLAID);
        return;
    }

    if( !inGraph(targetP) || !targetP->blockP )
    {
        addSinkEdge(tableP, blockP, kind, OPTSK_NOTCODE);
        return;
    }

    edgeP = addFlowEdge(tableP, blockP, kind);
    edgeP->toP = targetP->blockP;
    edgeP->toBank = bank;
    edgeP->toAddr = addr;
    edgeP->crossbank = (bank != blockP->bank);
    ++targetP->blockP->predCount;
}

// Make one edge per via-pointer jump A3 derived for an indirect transfer.
// A pointer naming another bank is a 16-bit pointer and is only valid with
// extend mode on, which A3 recorded as OPTEF_CROSSBANK; the flow edge marks
// it too, from the banks themselves so the two can never disagree.
// Returns how many edges were made, 0 when A3 could not follow the pointer.
static int
viaJumpEdges(OptTableP tableP, OptBlockP blockP, OptFlowKind kind, OptWordP entryP)
{
OptEdgeP edgeP;
int made;

    made = 0;

    for( edgeP = entryP->outP; edgeP; edgeP = edgeP->nextOutP )
    {
        if( (edgeP->role != OPTR_JUMP) || !(edgeP->flags & OPTEF_VIAPOINTER) )
        {
            continue;
        }

        addTargetEdge(tableP, blockP, kind, edgeP->toBank, edgeP->toAddr);
        ++made;
    }

    return(made);
}

// Make the edges for a jmp or a jsp: the rules are the same and only the edge
// kind differs.
static void
addTransferEdges(OptTableP tableP, OptBlockP blockP, OptFlowKind kind, OptWordP entryP)
{
    // A dap or dip writes this word's address field, so where it goes is a
    // run-time fact and no static target is the right one.
    if( entryP->flags & OPTF_PATCHED )
    {
        addSinkEdge(tableP, blockP, kind, OPTSK_PATCHED);
        return;
    }

    if( entryP->decode.memIndirect )
    {
        // DEBREAK.  "jmp i 1" executed in bank 0 returns from a sequence-break
        // handler, and location 1 holds the packed PC the hardware stored at
        // the break -- the address to return to (project owner, 08-Sep-2026;
        // the pdp1-emulator skill's references/sbs.md, "DEBREAK").  The target
        // is therefore whatever the break interrupted, a different address on
        // every break, and the word the source assembled into location 1
        // (conventionally 0) says nothing whatever about it.  Recognized only
        // in bank 0, which is where the hardware recognizes it.
        if( (entryP->bank == 0) && (entryP->decode.address == OPTDEBREAK_ADDR) )
        {
            addSinkEdge(tableP, blockP, kind, OPTSK_DEBREAK);
            return;
        }

        if( !viaJumpEdges(tableP, blockP, kind, entryP) )
        {
            addSinkEdge(tableP, blockP, kind, OPTSK_INDIRECT);
        }

        return;
    }

    addTargetEdge(tableP, blockP, kind, entryP->bank, entryP->decode.address);
}

// Pass 5: make the successor edges of one block, from its last word.  A block
// that did not end on a terminator falls through to the next address, and the
// fall-through finds the sink on its own when there is nothing to fall into.
static void
buildBlockEdges(OptTableP tableP, OptBlockP blockP)
{
OptWordP lastP;
int next;
int skipTo;
int addr;

    lastP = blockP->lastP;
    addr = lastP->decode.address;

    // The PDP-1's address is twelve bits, so running off the top of a bank
    // wraps to the bottom of the same bank rather than entering the next one.
    next = ((blockP->endAddr + 1) & ADDRMASK);
    skipTo = ((blockP->endAddr + 2) & ADDRMASK);

    switch( blockP->endKind )
    {
    case OPTBE_SKIP:
        addTargetEdge(tableP, blockP, OPTFK_FALL, blockP->bank, next);
        addTargetEdge(tableP, blockP, OPTFK_SKIP, blockP->bank, skipTo);
        break;

    case OPTBE_JUMP:
        addTransferEdges(tableP, blockP, OPTFK_JUMP, lastP);
        break;

    case OPTBE_CALL:
        if( lastP->decode.opcode == 016 )
        {
            if( lastP->flags & OPTF_PATCHED )
            {
                addSinkEdge(tableP, blockP, OPTFK_CALL, OPTSK_PATCHED);
            }
            else if( lastP->decode.indirect )
            {
                // jda deposits AC in the word it names and enters the word
                // after it.  jda never indirects: bit 5 is part of its
                // instruction code, which is why it is tested here and not
                // through addTransferEdges().
                addTargetEdge(tableP, blockP, OPTFK_CALL, lastP->bank, ((addr + 1) & ADDRMASK));
            }
            else
            {
                // cal is jda 0100: it ignores its address field entirely and
                // enters 0101 of its own bank.
                addTargetEdge(tableP, blockP, OPTFK_CALL, lastP->bank, CAL_JUMP_ADDR);
            }
        }
        else
        {
            addTransferEdges(tableP, blockP, OPTFK_CALL, lastP);
        }

        // The return is assumed, never proved: a subroutine that never
        // returns, or returns somewhere else, still gets this edge.  Study
        // 5.4 calls it an assumed return edge for exactly that reason.
        addTargetEdge(tableP, blockP, OPTFK_RETURN, blockP->bank, next);
        break;

    case OPTBE_HALT:
        // Pressing Continue on the console resumes at the next word, so the
        // word after a halt is reachable and is not dead code.
        addTargetEdge(tableP, blockP, OPTFK_RESUME, blockP->bank, next);
        break;

    case OPTBE_BOUNDARY:
    case OPTBE_NOTCODE:
    case OPTBE_GAP:
    default:
        addTargetEdge(tableP, blockP, OPTFK_FALL, blockP->bank, next);
        break;
    }
}

// Pass 6: mark the entry words and the blocks they begin.  Every entry word
// is the first word of its block, because markBlockStarts() marked all four
// entry causes as block starts.
static void
markEntries(OptTableP tableP)
{
int i;
OptWordP entryP;
OptEntryCause cause;

    for( i = 0; i < tableP->count; ++i )
    {
        entryP = tableP->entriesPP[i];

        if( !entryP->blockP )
        {
            continue;
        }

        if( (cause = entryCause(entryP)) == OPTEN_COUNT )
        {
            continue;
        }

        entryP->flags |= OPTF_ENTRY;
        ++tableP->entryCount;
        ++tableP->entryCounts[cause];
        entryP->blockP->isEntry = 1;
    }
}

// Pass 7: walk reachability from the entry blocks over every non-sink edge.
// A depth-first walk with an explicit stack; each block is pushed at most
// once, so the stack can never need more slots than there are blocks.
static void
walkReachability(OptTableP tableP)
{
OptBlockP *stackPP;
OptBlockP blockP;
OptFlowEdgeP edgeP;
int top;

    if( !tableP->blockCount )
    {
        return;
    }

    if( !(stackPP = (OptBlockP *)calloc(tableP->blockCount, sizeof(OptBlockP))) )
    {
        fprintf(stderr, "am1: out of memory walking the optimizer control-flow graph\n");
        exit(1);
    }

    top = 0;

    for( blockP = tableP->blocksP; blockP; blockP = blockP->nextP )
    {
        if( blockP->isEntry )
        {
            blockP->reached = 1;
            stackPP[top++] = blockP;
        }
    }

    while( top > 0 )
    {
        blockP = stackPP[--top];

        for( edgeP = blockP->succP; edgeP; edgeP = edgeP->nextP )
        {
            // A sink edge means the analysis lost the thread, not that
            // control arrived anywhere, so it spreads no reachability.
            if( !edgeP->toP || edgeP->toP->reached )
            {
                continue;
            }

            edgeP->toP->reached = 1;
            stackPP[top++] = edgeP->toP;
        }
    }

    free(stackPP);

    for( blockP = tableP->blocksP; blockP; blockP = blockP->nextP )
    {
        if( blockP->reached )
        {
            ++tableP->reachedBlocks;
        }
        else
        {
            ++tableP->unreachedBlocks;
        }
    }
}

// Pass 8: flag the words in blocks nothing reached.  An xct'd word runs where
// it lies, without control ever being transferred to it, so "nothing enters
// this block" is the wrong thing to say about it; it is counted separately
// as executed only by xct.
static void
markUnreached(OptTableP tableP)
{
int i;
OptWordP entryP;

    for( i = 0; i < tableP->count; ++i )
    {
        entryP = tableP->entriesPP[i];

        if( !entryP->blockP || entryP->blockP->reached )
        {
            continue;
        }

        if( entryP->flags & OPTF_XCTTARGET )
        {
            entryP->flags |= OPTF_XCTONLY;
            ++tableP->xctOnlyWords;
        }
        else
        {
            entryP->flags |= OPTF_UNREACHED;
            ++tableP->unreachedWords;
        }
    }
}

// Does an unreached block hold at least one word that is genuinely unreached,
// rather than one an xct executes in place?  A block every word of which is
// xct-only is executed, and listing it as apparently unreached code would be
// simply wrong.
// Returns 1 when the block has an unreached word, 0 when it has none.
static int
blockHasUnreachedWord(OptTableP tableP, OptBlockP blockP)
{
int addr;
OptWordP entryP;

    for( addr = blockP->startAddr; addr <= blockP->endAddr; ++addr )
    {
        entryP = wordAt(tableP, blockP->bank, addr);

        if( entryP && (entryP->flags & OPTF_UNREACHED) )
        {
            return(1);
        }
    }

    return(0);
}

// Release every block and its successor edges, and clear the table's list.
// Safe to call on a table whose flow was never built.
static void
freeBlockList(OptTableP tableP)
{
OptBlockP blockP;
OptBlockP nextBlockP;
OptFlowEdgeP edgeP;
OptFlowEdgeP nextEdgeP;

    for( blockP = tableP->blocksP; blockP; blockP = nextBlockP )
    {
        nextBlockP = blockP->nextP;

        for( edgeP = blockP->succP; edgeP; edgeP = nextEdgeP )
        {
            nextEdgeP = edgeP->nextP;
            free(edgeP);
        }

        free(blockP);
    }

    tableP->blocksP = NILP;
    tableP->blocksTailP = NILP;
}

// Name a control-flow edge kind.
// Returns a static string, never NILP.
const char *
optFlowKindName(OptFlowKind kind)
{
    switch( kind )
    {
    case OPTFK_FALL:
        return("fall");

    case OPTFK_SKIP:
        return("skip");

    case OPTFK_JUMP:
        return("jump");

    case OPTFK_CALL:
        return("call");

    case OPTFK_RETURN:
        return("return");

    case OPTFK_RESUME:
        return("resume");

    default:
        return("unknown");
    }
}

// Name the way a block ends.
// Returns a static string, never NILP.
const char *
optBlockEndName(OptBlockEnd end)
{
    switch( end )
    {
    case OPTBE_SKIP:
        return("skip");

    case OPTBE_JUMP:
        return("jump");

    case OPTBE_CALL:
        return("call");

    case OPTBE_HALT:
        return("halt");

    case OPTBE_BOUNDARY:
        return("boundary");

    case OPTBE_NOTCODE:
        return("notcode");

    case OPTBE_GAP:
        return("gap");

    default:
        return("unknown");
    }
}

// Name the reason an edge went to the UNKNOWN sink.
// Returns a static string, never NILP.
const char *
optSinkName(OptSink sink)
{
    switch( sink )
    {
    case OPTSK_NONE:
        return("none");

    case OPTSK_INDIRECT:
        return("indirect");

    case OPTSK_PATCHED:
        return("patched");

    case OPTSK_NOWORD:
        return("noword");

    case OPTSK_NOTCODE:
        return("notcode");

    case OPTSK_OVERLAID:
        return("overlaid");

    case OPTSK_DEBREAK:
        return("debreak");

    default:
        return("unknown");
    }
}

// Name why a word is an entry of the reachability walk.
// Returns a static string, never NILP.
const char *
optEntryCauseName(OptEntryCause cause)
{
    switch( cause )
    {
    case OPTEN_START:
        return("start");

    case OPTEN_TAKEN:
        return("taken");

    case OPTEN_EXPORT:
        return("export");

    case OPTEN_SBS:
        return("sbs");

    default:
        return("unknown");
    }
}

// Print the control-flow dump (-O=flow), in three parts.
//
// First one line per entry, in emission order, reserved words included:
//
//   BBAAAA VVVVVV kind    block N    FLAGS
//
// BBAAAA, VVVVVV and kind are as in the reference dump (------ for a reserved
// word's value); the block id is "-" for a word the graph does not hold;
// FLAGS is the word's flow flags in the order entry, blockstart, label,
// afterskip, unreached, xctonly, or "-" when it has none.
//
// Then one line per block, in bank then address order:
//
//   block N: BBAAAA-BBAAAA W words, P in, ends END STATE | succ: EDGE, EDGE
//
// where END is how the block ends, STATE is "entry reached", "reached" or
// "unreached", P is how many non-sink edges arrive, and an edge is either
// KIND->BBAAAA with " crossbank" appended when it leaves the bank, or
// KIND->sink:REASON.  (The task's design sketched the sink as a target plus a
// reason; naming only the reason is what actually reads unambiguously, since
// half the sink reasons have no meaningful address to print.)  The successor
// list is "-" when the block has none, which cannot happen but is printed
// rather than assumed.
//
// Then the entry set and the unreached words, both in emission order:
//
//   entry set: BBAAAA BBAAAA ...
//   unreached: BBAAAA ...
//
// each "none" when empty.  Everything here is hand-writable, which is what
// Tests/Flow/flow_test.expected relies on.
void
optDumpFlow(FILE *fP, OptTableP tableP)
{
int i;
int f;
int any;
char idBuf[16];
OptWordP entryP;
OptBlockP blockP;
OptFlowEdgeP edgeP;

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

        if( entryP->blockP )
        {
            snprintf(idBuf, sizeof(idBuf), "%d", entryP->blockP->id);
        }
        else
        {
            snprintf(idBuf, sizeof(idBuf), "-");
        }

        fprintf(fP, "block %-4s ", idBuf);

        any = 0;

        for( f = 0; f < (int)(sizeof(flowFlagNames) / sizeof(flowFlagNames[0])); ++f )
        {
            if( entryP->flags & flowFlagNames[f].bit )
            {
                fprintf(fP, "%s%s", (any)?" ":"", flowFlagNames[f].nameP);
                any = 1;
            }
        }

        if( !any )
        {
            fprintf(fP, "-");
        }

        fprintf(fP, "\n");
    }

    for( blockP = tableP->blocksP; blockP; blockP = blockP->nextP )
    {
        fprintf(fP, "block %d: %02o%04o-%02o%04o %d words, %d in, ends %s %s%s | succ:",
            blockP->id,
            blockP->bank, blockP->startAddr, blockP->bank, blockP->endAddr,
            blockP->wordCount, blockP->predCount,
            optBlockEndName(blockP->endKind),
            (blockP->isEntry)?"entry ":"",
            (blockP->reached)?"reached":"unreached");

        any = 0;

        for( edgeP = blockP->succP; edgeP; edgeP = edgeP->nextP )
        {
            fprintf(fP, (any)?", ":" ");

            if( edgeP->toP )
            {
                fprintf(fP, "%s->%02o%04o%s", optFlowKindName(edgeP->kind),
                    edgeP->toBank, edgeP->toAddr, (edgeP->crossbank)?" crossbank":"");
            }
            else
            {
                fprintf(fP, "%s->sink:%s", optFlowKindName(edgeP->kind), optSinkName(edgeP->sink));
            }

            any = 1;
        }

        if( !any )
        {
            fprintf(fP, " -");
        }

        fprintf(fP, "\n");
    }

    fprintf(fP, "entry set:");
    any = 0;

    for( i = 0; i < tableP->count; ++i )
    {
        entryP = tableP->entriesPP[i];

        if( entryP->flags & OPTF_ENTRY )
        {
            fprintf(fP, " %02o%04o", entryP->bank, entryP->addr);
            any = 1;
        }
    }

    fprintf(fP, "%s\n", (any)?"":" none");

    fprintf(fP, "unreached:");
    any = 0;

    for( i = 0; i < tableP->count; ++i )
    {
        entryP = tableP->entriesPP[i];

        if( entryP->flags & OPTF_UNREACHED )
        {
            fprintf(fP, " %02o%04o", entryP->bank, entryP->addr);
            any = 1;
        }
    }

    fprintf(fP, "%s\n", (any)?"":" none");
}

// Write one bank's control-flow section of the report: what the graph holds
// there, the edges it made, and what the reachability walk found.  Silent
// about a bank with no code words beyond saying so, since every figure would
// be zero.
static void
writeFlowReport(FILE *fP, OptTableP tableP, int bank)
{
int i;
int blocks;
int words;
int largest;
int entries;
int reached;
int unreachedBlocks;
int unreachedWords;
int xctOnly;
int any;
int kindCounts[OPTFK_COUNT];
int sinkCounts[OPTSK_COUNT];
int entryCounts[OPTEN_COUNT];
OptBlockP blockP;
OptFlowEdgeP edgeP;
OptWordP entryP;
OptEntryCause cause;

    blocks = 0;
    words = 0;
    largest = 0;
    entries = 0;
    reached = 0;
    unreachedBlocks = 0;
    unreachedWords = 0;
    xctOnly = 0;

    for( i = 0; i < OPTFK_COUNT; ++i )
    {
        kindCounts[i] = 0;
    }

    for( i = 0; i < OPTSK_COUNT; ++i )
    {
        sinkCounts[i] = 0;
    }

    for( i = 0; i < OPTEN_COUNT; ++i )
    {
        entryCounts[i] = 0;
    }

    for( blockP = tableP->blocksP; blockP; blockP = blockP->nextP )
    {
        if( blockP->bank != bank )
        {
            continue;
        }

        ++blocks;
        words += blockP->wordCount;

        if( blockP->wordCount > largest )
        {
            largest = blockP->wordCount;
        }

        if( blockP->reached )
        {
            ++reached;
        }
        else
        {
            ++unreachedBlocks;
        }

        for( edgeP = blockP->succP; edgeP; edgeP = edgeP->nextP )
        {
            ++kindCounts[edgeP->kind];

            if( !edgeP->toP )
            {
                ++sinkCounts[edgeP->sink];
            }
        }
    }

    for( i = 0; i < tableP->count; ++i )
    {
        entryP = tableP->entriesPP[i];

        if( entryP->bank != bank )
        {
            continue;
        }

        if( entryP->flags & OPTF_ENTRY )
        {
            ++entries;

            if( (cause = entryCause(entryP)) != OPTEN_COUNT )
            {
                ++entryCounts[cause];
            }
        }

        if( entryP->flags & OPTF_UNREACHED )
        {
            ++unreachedWords;
        }

        if( entryP->flags & OPTF_XCTONLY )
        {
            ++xctOnly;
        }
    }

    if( !blocks )
    {
        fprintf(fP, "    flow: no code words, no blocks\n");
        return;
    }

    fprintf(fP, "    flow: %d block%s, %d word%s, largest %d; edges:",
        blocks, (blocks == 1)?"":"s", words, (words == 1)?"":"s", largest);

    for( i = 0; i < OPTFK_COUNT; ++i )
    {
        if( kindCounts[i] )
        {
            fprintf(fP, " %s %d", optFlowKindName((OptFlowKind)i), kindCounts[i]);
        }
    }

    any = 0;

    for( i = 0; i < OPTSK_COUNT; ++i )
    {
        if( sinkCounts[i] )
        {
            fprintf(fP, "%s %s %d", (any)?"":"; to the unknown sink:",
                optSinkName((OptSink)i), sinkCounts[i]);
            any = 1;
        }
    }

    fprintf(fP, "\n    reachability: %d entries", entries);

    for( i = 0; i < OPTEN_COUNT; ++i )
    {
        if( entryCounts[i] )
        {
            fprintf(fP, " %s %d", optEntryCauseName((OptEntryCause)i), entryCounts[i]);
        }
    }

    fprintf(fP, "; %d blocks reached, %d unreached holding %d word%s",
        reached, unreachedBlocks, unreachedWords, (unreachedWords == 1)?"":"s");

    if( xctOnly )
    {
        fprintf(fP, ", %d word%s executed only by xct", xctOnly, (xctOnly == 1)?"":"s");
    }

    fprintf(fP, "\n");
}

// Write the apparently-unreached section at the end of the report, one line
// per unreached block, with the caveat that makes the list readable: it is a
// hint and there are several ordinary reasons a live routine lands in it.
// Blocks whose every word is executed by an xct are not listed; they are
// executed, and calling them unreached code would simply be wrong.
static void
writeUnreachedList(FILE *fP, OptTableP tableP)
{
int blocks;
OptBlockP blockP;

    blocks = 0;

    for( blockP = tableP->blocksP; blockP; blockP = blockP->nextP )
    {
        if( !blockP->reached && blockHasUnreachedWord(tableP, blockP) )
        {
            ++blocks;
        }
    }

    if( !blocks )
    {
        return;
    }

    fprintf(fP, "\nApparently unreached code: %d block%s, %d word%s\n",
        blocks, (blocks == 1)?"":"s",
        tableP->unreachedWords, (tableP->unreachedWords == 1)?"":"s");
    fprintf(fP, "  A hint, not a proof.  A dispatch table of jmp words is read as data and\n");
    fprintf(fP, "  is never followed as a jump, so a routine only ever entered through one\n");
    fprintf(fP, "  appears here; so does anything reached only through a pointer that could\n");
    fprintf(fP, "  not be followed, or through a patched address field.  Check against the\n");
    fprintf(fP, "  source before believing any of it.\n");
    // Task A4 measured where this list's noise comes from and asked this task
    // to say so here.  The check it would take -- proving a callee steps its
    // return word past an inline argument -- is interprocedural and belongs to
    // a later task, so the list carries the caveat instead of the check.
    fprintf(fP, "  The commonest cause by far is the inline argument: a call whose argument\n");
    fprintf(fP, "  word follows it, and a callee that returns past that word.  The return\n");
    fprintf(fP, "  edge here is assumed to go to the word after the call, which is the\n");
    fprintf(fP, "  argument, which is data, so the return is lost and everything the caller\n");
    fprintf(fP, "  would have reached is called unreached.  Proving otherwise needs to look\n");
    fprintf(fP, "  inside the callee, which nothing here does.  One more caveat the other\n");
    fprintf(fP, "  way: an xct is not treated as a terminator, so a word after an xct of a\n");
    fprintf(fP, "  jump is called reached when it may never run.\n");

    for( blockP = tableP->blocksP; blockP; blockP = blockP->nextP )
    {
        if( blockP->reached || !blockHasUnreachedWord(tableP, blockP) )
        {
            continue;
        }

        fprintf(fP, "    bank %2d %04o-%04o, %d word%s, line %d, first label %s\n",
            blockP->bank, blockP->startAddr, blockP->endAddr,
            blockP->wordCount, (blockP->wordCount == 1)?"":"s",
            blockP->firstP->lineNo, firstLabelName(blockP->firstP));
    }
}

// ===========================================================================
// Task A5: instruction timing, the preconditions, the rules, the findings
// and the report they exist to produce.
//
// A rule walks the words of one bank in address order and, wherever its
// pattern matches, builds a finding: the words involved, the source it
// suggests instead, and what that would save.  Before the finding is called
// live it has to pass the shared preconditions of study section 6 and then
// whatever the rule itself demands of the words' contents.  A pattern that
// matches and is then refused becomes a suppressed finding carrying its
// reason, which study section 7 calls the more informative half.
//
// Nothing here changes a word.  The parse tree, the symbol tables and the
// tape are exactly what they would have been without -O.
// ===========================================================================

// Instruction times, in microseconds, from the F-15D handbook.  "Operating
// Speeds" gives the rule: the memory cycle is 5 microseconds; a two-cycle
// instruction, one that refers to memory twice, takes 10 ("add, subtract,
// deposit, load, etc."); "the jump, augmented and combined augmented
// instructions need only one call on memory and are performed in 5"; an
// in-out transfer without the wait function takes 5, and with it "the
// operating time depends upon the device being used"; and "each step of
// indirect addressing requires an additional 5".  The appendix's Abbreviated
// Instruction List gives the same figure against every instruction, and adds
// "25 max" for mul and "40 max" for div.  The maxima are used here: a static
// sum cannot know whether a particular divide will overflow (12) or run the
// full 40, and the report says which way it rounded.
#define OPTTIME_CYCLE       5       // one memory cycle, and every augmented instruction
#define OPTTIME_MEMREF      10      // two cycles: fetch the instruction, reach the operand
#define OPTTIME_MUL         25      // handbook appendix, "25 max"
#define OPTTIME_DIV         40      // handbook appendix, "40 max"

// The largest buffer any spelling or evidence string is built in.  A symbol
// name can be 1023 characters and an expression several of them, so a long
// one is truncated rather than allowed to overflow; nothing here is parsed
// again, so a truncated spelling costs readability and nothing else.
#define OPTMSG_SIZE         1024

// Which of the shared preconditions a rule asks for.  A rule that removes
// words needs all four; one that rewrites a word in place without moving
// anything needs only the ones that say the word is what it appears to be.
#define OPTPC_P1            0x01    // no side entry into an interior word
#define OPTPC_P2            0x02    // no word of the pattern written or taken
#define OPTPC_P3            0x04    // no word of the pattern executed by an xct
#define OPTPC_P4            0x08    // no skip-class word immediately before it
#define OPTPC_ALL           0x0F

// The micro-ops whose effect done twice is not their effect done once, so
// two words that both carry one cannot be merged into a word that carries it
// once: two halts are two stops, and two complements cancel.
static const OptMicroId optNotIdempotent[3] = { OPTMO_HLT, OPTMO_CMA, OPTMO_CMI };

// The rules, their short names and what each one looks for.  The order is
// the order the report groups findings in.
static const struct
{
    const char *nameP;
    const char *summaryP;
} optRuleTable[OPTRULE_COUNT] =
{
    { "T1",  "two consecutive operate words merge into one" },
    { "T1b", "two consecutive shifts of the same opcode merge into one" },
    { "T2",  "skip; jmp .+2; W is the inverted skip and W" },
    { "T3",  "a jmp to a word that is itself a jmp can go straight to the far target" },
    { "T6",  "a cla before an instruction that loads AC outright" },
    { "T7",  "a cli before a lio" },
    { "T8a", "lio of a pool word holding zero is cli" },
    { "T8b", "lac of a pool word holding zero is cla" },
    { "T8c", "lac of a pool word holding all ones is cla cma" },
    { "T8d", "lac of a pool word holding a 12 bit value is a law immediate" },
    { "T13", "a jmp to the next word does nothing" },
    { "T14", "a copy between AC and IO through a temporary is a PDP-1D transfer" }
};

// Why a matched pattern was refused: the tag the dump prints and the sentence
// the report prints.
static const struct
{
    const char *tagP;
    const char *textP;
} optWhyTable[OPTWHY_COUNT] =
{
    { "-",          "not refused" },
    { "P1-label",   "P1: a word inside the pattern carries a label" },
    { "P1-entry",   "P1: a word inside the pattern is a jump target or a program entry" },
    { "P2-written", "P2: a word of the pattern is written while the program runs" },
    { "P2-taken",   "P2: the address of a word of the pattern is used as a value" },
    { "P3-xct",     "P3: a word of the pattern is executed by an xct" },
    { "P4-skip",    "P4: a skip-class word sits immediately before the pattern" },
    { "phase",      "the hardware would apply the merged micro-ops in the other order" },
    { "flags",      "both words operate on a program flag, and one word has only one flag field" },
    { "repeat",     "a micro-op that is not idempotent appears in both words" },
    { "lap",        "the second word carries lap, whose value is its own address plus one" },
    { "exchange",   "lia and lai in separate words are two copies, not one exchange" },
    { "count",      "the summed shift count is more than the nine a shift word can hold" },
    { "noinverse",  "the skip-class word has no inverted form" },
    { "patched",    "the word the rule reads through has its address field written at run time" },
    { "tempused",   "the temporary is reached from outside the pattern" }
};

static char *allocPrintf(const char *fmtP, ...);
static int appendStr(char *bufP, int size, int at, const char *strP);
static int appendOctal(char *bufP, int size, int at, int value);
static const char *binopText(int op);
static int spellExpr(char *bufP, int size, int at, PNodeP nodeP);
static int spellOperandExpr(char *bufP, int size, int at, PNodeP nodeP);
static void spellWord(OptWordP entryP, char *bufP, int size);
static void spellOperand(OptWordP entryP, char *bufP, int size);
static void spellOperate(unsigned int microBits, char *bufP, int size);
static void spellSkipValue(int value, char *bufP, int size);

static OptWordP patternWord(OptTableP tableP, int bank, int addr);
static OptFindingP startFinding(OptRuleId rule, OptWordP *wordsPP, int count);
static void emitFinding(OptTableP tableP, OptFindingP findingP, OptWhy why);
static void describeReferrer(OptWordP entryP, OptRole role, int patchOnly, char *bufP, int size);

static OptWhy checkP1(OptFindingP findingP);
static OptWhy checkP2(OptFindingP findingP);
static OptWhy checkP3(OptFindingP findingP);
static OptWhy checkP4(OptTableP tableP, OptFindingP findingP);
static OptWhy checkShared(OptTableP tableP, OptFindingP findingP, unsigned int which);
static int isDivWord(OptWordP entryP);
static int isMemrefOp(OptWordP entryP, int opcode);

static int t6Claims(OptWordP w1P, OptWordP w2P);
static int t7Claims(OptWordP w1P, OptWordP w2P);
static OptWhy t1Refusal(OptWordP w1P, OptWordP w2P, OptFindingP findingP);
static int invertSkip(OptWordP entryP, int *valueP);

static void ruleT1(OptTableP tableP, int bank, int addr);
static void ruleT1b(OptTableP tableP, int bank, int addr);
static void ruleT2(OptTableP tableP, int bank, int addr);
static void ruleT3(OptTableP tableP, int bank, int addr);
static void ruleT6(OptTableP tableP, int bank, int addr);
static void ruleT7(OptTableP tableP, int bank, int addr);
static void ruleT8(OptTableP tableP, int bank, int addr);
static void ruleT13(OptTableP tableP, int bank, int addr);
static void ruleT14(OptTableP tableP, int bank, int addr, int *claimedToP);
static int tempIsPrivate(OptTableP tableP, OptFindingP findingP, OptWordP tempP);

static void printFinding(FILE *fP, OptFindingP findingP, int suppressed);

// Run every rule over a table whose control flow has been overlaid.  The scan
// is by bank and then by address, and each rule appends its findings as it
// meets them, so both lists come out sorted by bank then address without ever
// being sorted: the report can be diffed between runs and used as a test
// reference.
//
// T6 and T7 go first at each address because they claim pairs that T1 would
// otherwise merge to exactly the same word; reporting one pair twice would
// double the saving in the totals.
void
optRunRules(OptTableP tableP)
{
int bank;
int addr;
int t14ClaimedTo;

    for( bank = 0; bank <= MAXBANK; ++bank )
    {
        if( !tableP->banksP[bank] )
        {
            continue;
        }

        // The highest address a T14 exchange has already taken, so the two
        // word forms do not report a fragment of one as a finding of their
        // own.  Addresses only rise inside a bank, so one mark is enough.
        t14ClaimedTo = -1;

        for( addr = 0; addr < BANKSIZE; ++addr )
        {
            ruleT6(tableP, bank, addr);
            ruleT7(tableP, bank, addr);
            ruleT1(tableP, bank, addr);
            ruleT1b(tableP, bank, addr);
            ruleT2(tableP, bank, addr);
            ruleT3(tableP, bank, addr);
            ruleT8(tableP, bank, addr);
            ruleT13(tableP, bank, addr);
            ruleT14(tableP, bank, addr, &t14ClaimedTo);
        }
    }
}

// The static execution time of one word in microseconds, from the handbook
// figures at the top of this section.  Three things it cannot include, all of
// them noted where the report prints the sum: the instruction an xct runs, the
// wait an iot with bit 5 set may sit in, and any indirect step past the first.
// Returns the time, or 0 for a reserved word or a spare instruction code.
int
optWordTime(OptWordP entryP)
{
int time;

    if( !entryP || (entryP->flags & OPTF_RESERVED) )
    {
        return(0);
    }

    switch( entryP->decode.group )
    {
    case OPTG_LAW:
    case OPTG_SKIP:
    case OPTG_SHIFT:
    case OPTG_OPERATE:
    case OPTG_1D:
    case OPTG_IOT:
        // The augmented instructions and an iot that does not wait: one call
        // on memory, so one cycle.
        return(OPTTIME_CYCLE);

    case OPTG_MEMREF:
        break;

    case OPTG_UNKNOWN:
    default:
        // A spare instruction code.  What the machine does with it is not
        // defined, so there is no time to give it.
        return(0);
    }

    switch( entryP->decode.opcode )
    {
    case 010:       // xct: one cycle, plus the instruction it runs, which is
                    // another word's time and is counted against that word
    case 060:       // jmp
    case 062:       // jsp
        time = OPTTIME_CYCLE;
        break;

    case 054:       // mul
        time = OPTTIME_MUL;
        break;

    case 056:       // div
        time = OPTTIME_DIV;
        break;

    default:
        // Every other memory reference.
        time = OPTTIME_MEMREF;
        break;
    }

    if( entryP->decode.memIndirect )
    {
        // One step of indirect addressing.  A pointer that itself indirects
        // costs another cycle and cannot be seen from here.
        time += OPTTIME_CYCLE;
    }

    return(time);
}

// Name a rule for the report and the dump.
// Returns a static string, never NILP.
const char *
optRuleName(OptRuleId rule)
{
    if( (rule < 0) || (rule >= OPTRULE_COUNT) )
    {
        return("?");
    }

    return( optRuleTable[rule].nameP );
}

// The one line description of a rule, for the report's group headings.
// Returns a static string, never NILP.
const char *
optRuleSummary(OptRuleId rule)
{
    if( (rule < 0) || (rule >= OPTRULE_COUNT) )
    {
        return("unknown rule");
    }

    return( optRuleTable[rule].summaryP );
}

// The short tag for a refusal reason.
// Returns a static string, never NILP.
const char *
optWhyName(OptWhy why)
{
    if( (why < 0) || (why >= OPTWHY_COUNT) )
    {
        return("?");
    }

    return( optWhyTable[why].tagP );
}

// ---------------------------------------------------------------------------
// Strings: building the spellings and the evidence sentences.
// ---------------------------------------------------------------------------

// Format a string into freshly allocated memory.  Truncates at OPTMSG_SIZE;
// out of memory is fatal, as it is everywhere else in am1.
// Returns the string, which the caller owns.
static char *
allocPrintf(const char *fmtP, ...)
{
va_list args;
char buf[OPTMSG_SIZE];
char *strP;

    va_start(args, fmtP);
    vsnprintf(buf, sizeof(buf), fmtP, args);
    va_end(args);

    if( !(strP = (char *)malloc(strlen(buf) + 1)) )
    {
        fprintf(stderr, "am1: out of memory building an optimizer finding\n");
        exit(1);
    }

    strcpy(strP, buf);
    return(strP);
}

// Append a string to a bounded buffer at offset at, stopping at the end.
// Returns the new offset; the buffer is always terminated.
static int
appendStr(char *bufP, int size, int at, const char *strP)
{
    if( !strP )
    {
        return(at);
    }

    while( *strP && (at < (size - 1)) )
    {
        bufP[at++] = *strP++;
    }

    bufP[at] = '\0';
    return(at);
}

// Append an 18 bit value as an octal literal.  The 0o prefix is not
// decoration: am1 reads a bare number in whatever radix is current, so a
// suggested source line with a bare octal number in it would assemble to
// something else under 'decimal'.  The prefix makes the suggestion say what
// it means wherever it is pasted.
// Returns the new offset.
static int
appendOctal(char *bufP, int size, int at, int value)
{
char tmp[32];

    sprintf(tmp, "0o%o", (value & WRDMASK));
    return( appendStr(bufP, size, at, tmp) );
}

// The am1 source text of a binary operator.  A space is or, which is why
// SEPARATOR spells as one.
// Returns a static string, never NILP.
static const char *
binopText(int op)
{
    switch( op )
    {
    case PLUS:
        return("+");

    case MINUS:
        return("-");

    case MUL:
        return("*");

    case DIV:
        return("/");

    case MOD:
        return("%%");

    case AND:
        return("&");

    case OR:
        return("|");

    case XOR:
        return("^");

    case LSHIFT:
        return("<<");

    case RSHIFT:
        return(">>");

    case SEPARATOR:
        return(" ");

    default:
        return(" ? ");
    }
}

// Render an expression tree as am1 source.  This is emitOperand() from
// maccodegen.c written for am1's own syntax rather than macro1's, and
// without the reductions macro1 needs: a constant stays [..], a complement
// stays ~, and a symbol keeps its name.
// Returns the new offset.
static int
spellExpr(char *bufP, int size, int at, PNodeP nodeP)
{
SymNodeP symP;
char tmp[64];

    if( !nodeP )
    {
        return(at);
    }

    switch( nodeP->type )
    {
    case BINOP:
        at = spellExpr(bufP, size, at, nodeP->leftP);
        at = appendStr(bufP, size, at, binopText(nodeP->value.ival));
        at = spellExpr(bufP, size, at, nodeP->rightP);
        break;

    case UNOP:
        switch( nodeP->value.ival )
        {
        case PARENS:
            at = appendStr(bufP, size, at, "(");
            at = spellExpr(bufP, size, at, nodeP->rightP);
            at = appendStr(bufP, size, at, ")");
            break;

        case UMINUS:
            at = appendStr(bufP, size, at, "-");
            at = spellExpr(bufP, size, at, nodeP->rightP);
            break;

        case CMPL:
            at = appendStr(bufP, size, at, "~");
            at = spellExpr(bufP, size, at, nodeP->rightP);
            break;

        case LAW:
            at = appendStr(bufP, size, at, "law ");
            at = spellExpr(bufP, size, at, nodeP->rightP);
            break;

        default:
            at = appendStr(bufP, size, at, "?");
            break;
        }
        break;

    case CONSTANT:
        // value2 holds this reference's own expression.  The pooled symbol
        // holds only the expression that landed in the slot first, and
        // literals are pooled by value, so reading the symbol would spell
        // some other reference's text.
        at = appendStr(bufP, size, at, "[");
        at = spellExpr(bufP, size, at, (PNodeP)(nodeP->value2.ptr));
        at = appendStr(bufP, size, at, "]");
        break;

    case DOT:
        at = appendStr(bufP, size, at, ".");
        break;

    case LAW:
    case OPADDR:
    case OPCODE:
    case OPORABLE:
    case VALUESPEC:
    case IMOD:
    case ADDR:
    case LCLADDR:
        symP = nodeP->value.symP;
        at = appendStr(bufP, size, at, (symP && symP->name)?symP->name:"?");
        break;

    case BREF:
        symP = nodeP->value.symP;
        at = appendStr(bufP, size, at, (symP && symP->name)?symP->name:"?");
        sprintf(tmp, ":%d", nodeP->value2.ival);
        at = appendStr(bufP, size, at, tmp);
        break;

    case WILDREF:
        at = appendStr(bufP, size, at, (nodeP->value.strP)?nodeP->value.strP:"?");
        at = appendStr(bufP, size, at, ":*");
        break;

    case CHAR:
    case FLEXO:
    case LITCHAR:
    case INTEGER:
        at = appendOctal(bufP, size, at, nodeP->value.ival);
        break;

    default:
        break;
    }

    return(at);
}

// Render only the operand part of an expression: everything except the
// mnemonic that named the instruction and the i modifier that goes with it.
// A binary operator is dropped along with whichever side contributed nothing,
// so "sad foo" spells as "foo" and not as " foo".
// Returns the new offset.
static int
spellOperandExpr(char *bufP, int size, int at, PNodeP nodeP)
{
char rightBuf[OPTMSG_SIZE];
int mid;

    if( !nodeP )
    {
        return(at);
    }

    switch( nodeP->type )
    {
    case OPADDR:
    case OPCODE:
    case OPORABLE:
    case LAW:
    case IMOD:
        // The mnemonic and its modifiers: not part of the operand.
        return(at);

    case BINOP:
        mid = spellOperandExpr(bufP, size, at, nodeP->leftP);
        rightBuf[0] = '\0';

        if( !spellOperandExpr(rightBuf, sizeof(rightBuf), 0, nodeP->rightP) )
        {
            return(mid);
        }

        if( mid > at )
        {
            mid = appendStr(bufP, size, mid, binopText(nodeP->value.ival));
        }

        return( appendStr(bufP, size, mid, rightBuf) );

    case UNOP:
        if( nodeP->value.ival == LAW )
        {
            return( spellOperandExpr(bufP, size, at, nodeP->rightP) );
        }
        break;

    default:
        break;
    }

    return( spellExpr(bufP, size, at, nodeP) );
}

// Render a word the way its source spelled it.  A word with no expression
// tree -- a text, ascii or type340 word, a reserved table word, a variable
// with no initializer -- has no source spelling at all; its symbol's name
// stands in, and failing that a dash.
static void
spellWord(OptWordP entryP, char *bufP, int size)
{
int at;

    bufP[0] = '\0';
    at = 0;

    if( entryP->exprP )
    {
        at = spellExpr(bufP, size, 0, entryP->exprP);
    }

    if( !at && entryP->symP && entryP->symP->name )
    {
        at = appendStr(bufP, size, 0, entryP->symP->name);
    }

    if( !at )
    {
        appendStr(bufP, size, 0, "-");
    }
}

// Render just the operand of a word, for a suggestion that keeps the operand
// and changes the mnemonic.
static void
spellOperand(OptWordP entryP, char *bufP, int size)
{
int at;

    bufP[0] = '\0';
    at = 0;

    if( entryP->exprP )
    {
        at = spellOperandExpr(bufP, size, 0, entryP->exprP);
    }

    if( !at )
    {
        // The source wrote no symbol for the field (lac 100, or a word whose
        // bits happen to decode as a memory reference).  The decoded address
        // is what the machine will use.
        appendOctal(bufP, size, 0, entryP->decode.address);
    }
}

// Render a set of operate micro-ops as am1 source, in the order the hardware
// applies them.  An operate word with no micro-ops at all is nop.
static void
spellOperate(unsigned int microBits, char *bufP, int size)
{
const OptMicroOp *opsP[OPTMO_COUNT];
int count;
int i;
int at;
char tmp[32];

    bufP[0] = '\0';
    at = 0;
    count = optOperateOrder(microBits, opsP, OPTMO_COUNT);

    for( i = 0; i < count; ++i )
    {
        if( at )
        {
            at = appendStr(bufP, size, at, " ");
        }

        if( (opsP[i]->id == OPTMO_CLF) || (opsP[i]->id == OPTMO_STF) )
        {
            // The flag field carries the flag number as its operand.
            sprintf(tmp, "%s %o", opsP[i]->nameP, (microBits & OPTM_FLAGNUM));
            at = appendStr(bufP, size, at, tmp);
        }
        else
        {
            at = appendStr(bufP, size, at, opsP[i]->nameP);
        }
    }

    if( !at )
    {
        appendStr(bufP, size, 0, "nop");
    }
}

// Render a skip group word as am1 source from its bits: every condition it
// names, then the sense switch and program flag fields, then the i that
// reverses the whole thing.  Used for the inverted skip T2 suggests, where
// there is no source text to copy because the word does not exist yet.
static void
spellSkipValue(int value, char *bufP, int size)
{
OptDecode decode;
int at;
char tmp[32];

    optDecodeValue(value, &decode);
    bufP[0] = '\0';
    at = 0;

    if( decode.skipConds & OPTC_SNI )
    {
        at = appendStr(bufP, size, at, "sni ");
    }

    if( decode.skipConds & OPTC_SPI )
    {
        at = appendStr(bufP, size, at, "spi ");
    }

    if( decode.skipConds & OPTC_SZO )
    {
        at = appendStr(bufP, size, at, "szo ");
    }

    if( decode.skipConds & OPTC_SMA )
    {
        at = appendStr(bufP, size, at, "sma ");
    }

    if( decode.skipConds & OPTC_SPA )
    {
        at = appendStr(bufP, size, at, "spa ");
    }

    if( decode.skipConds & OPTC_SZA )
    {
        at = appendStr(bufP, size, at, "sza ");
    }

    if( decode.skipSwitch )
    {
        sprintf(tmp, "szs %o ", decode.skipSwitch);
        at = appendStr(bufP, size, at, tmp);
    }

    if( decode.skipFlag )
    {
        sprintf(tmp, "szf %o ", decode.skipFlag);
        at = appendStr(bufP, size, at, tmp);
    }

    if( !at )
    {
        at = appendStr(bufP, size, at, "skp ");
    }

    if( decode.skipInverted )
    {
        at = appendStr(bufP, size, at, "i");
    }

    // Trim the separator the last field left behind.
    while( (at > 0) && (bufP[at - 1] == ' ') )
    {
        bufP[--at] = '\0';
    }
}

// ---------------------------------------------------------------------------
// Findings and the shared preconditions.
// ---------------------------------------------------------------------------

// The single word at a bank and address that a rule may reason about: one
// emitted word, not reserved, not sharing its address with an overlay, and
// classified code by task A3.  inGraph() is the same test the control-flow
// overlay used, so a rule and the graph can never disagree about what is an
// instruction.
// Returns the word, or NILP when there is nothing there a rule may touch.
static OptWordP
patternWord(OptTableP tableP, int bank, int addr)
{
OptWordP entryP;

    if( !(entryP = wordAt(tableP, bank, addr)) )
    {
        return(NILP);
    }

    if( !inGraph(entryP) )
    {
        return(NILP);
    }

    return(entryP);
}

// Begin a finding for a pattern that has matched.  The words are in address
// order and the first one gives the finding its place in the report.
// Returns the finding; out of memory is fatal.
static OptFindingP
startFinding(OptRuleId rule, OptWordP *wordsPP, int count)
{
OptFindingP findingP;
int i;

    if( !(findingP = (OptFindingP)calloc(1, sizeof(OptFinding))) )
    {
        fprintf(stderr, "am1: out of memory building an optimizer finding\n");
        exit(1);
    }

    findingP->rule = rule;
    findingP->why = OPTWHY_NONE;
    findingP->bank = wordsPP[0]->bank;
    findingP->addr = wordsPP[0]->addr;
    findingP->wordCount = count;

    for( i = 0; (i < count) && (i < OPTFIND_MAXWORDS); ++i )
    {
        findingP->wordsP[i] = wordsPP[i];
    }

    return(findingP);
}

// File a finding: live when why is OPTWHY_NONE, suppressed otherwise.  Both
// lists are appended to, never sorted, so they stay in the scan's bank and
// address order.  A per-hop saving (T3, which removes no word and shortens
// only the path through it) is not added to the totals: the totals say what
// one pass through every finding would save, and a hop count is not that.
static void
emitFinding(OptTableP tableP, OptFindingP findingP, OptWhy why)
{
    findingP->why = why;

    if( why == OPTWHY_NONE )
    {
        if( tableP->findingsTailP )
        {
            tableP->findingsTailP->nextP = findingP;
        }
        else
        {
            tableP->findingsP = findingP;
        }

        tableP->findingsTailP = findingP;
        ++tableP->findingCount;
        ++tableP->findingCounts[findingP->rule];
        tableP->savedWords += findingP->saveWords;
        tableP->savedTemps += findingP->saveTemps;

        if( !findingP->perHop )
        {
            tableP->savedTime += findingP->saveTime;
        }

        return;
    }

    if( tableP->suppressedTailP )
    {
        tableP->suppressedTailP->nextP = findingP;
    }
    else
    {
        tableP->suppressedP = findingP;
    }

    tableP->suppressedTailP = findingP;
    ++tableP->suppressedCount;
    ++tableP->suppressedCounts[findingP->rule];
    ++tableP->whyCounts[why];
}

// Release a finding list and the two strings each finding owns.
static void
freeFindingList(OptFindingP findingP)
{
OptFindingP nextP;

    while( findingP )
    {
        nextP = findingP->nextP;

        if( findingP->replaceP )
        {
            free(findingP->replaceP);
        }

        if( findingP->detailP )
        {
            free(findingP->detailP);
        }

        free(findingP);
        findingP = nextP;
    }
}

// Name the first word that reaches an entry in a given role, for the evidence
// line of a refused finding.  patchOnly asks for a dap or dip specifically,
// which is what makes the difference between "written" and "patched".
// The buffer always ends up with a sentence in it: a flag can also come from
// the conservative marking of task A3, where no edge says where it came from.
static void
describeReferrer(OptWordP entryP, OptRole role, int patchOnly, char *bufP, int size)
{
OptEdgeP edgeP;
char spell[OPTMSG_SIZE];

    for( edgeP = entryP->inP; edgeP; edgeP = edgeP->nextInP )
    {
        if( edgeP->role != role )
        {
            continue;
        }

        if( patchOnly && !(edgeP->flags & OPTEF_PATCH) )
        {
            continue;
        }

        if( !edgeP->fromP )
        {
            continue;
        }

        spellWord(edgeP->fromP, spell, sizeof(spell));
        snprintf(bufP, size, "by the %s at bank %d %04o, line %d", spell,
            edgeP->fromP->bank, edgeP->fromP->addr, edgeP->fromP->lineNo);
        return;
    }

    snprintf(bufP, size, "through a pointer this analysis could not follow");
}

// Precondition P1 (study section 6): nothing may enter the pattern anywhere
// but at its first word.  A label is the obvious way in and the study names
// it; a jump edge and a program entry are the ways in that carry no label,
// and task A4 exists so that they can be seen.  The first word is exempt:
// the rewrite starts there and has the same effect from there.
//
// Being a block start is deliberately NOT the test.  A rule whose pattern
// opens with a skip (T2) makes block starts of its own following words, and
// testing for them would refuse every T2 there is.
// Returns OPTWHY_NONE when the precondition holds, the reason otherwise.
static OptWhy
checkP1(OptFindingP findingP)
{
int i;
OptWordP entryP;

    for( i = 1; i < findingP->wordCount; ++i )
    {
        entryP = findingP->wordsP[i];

        if( entryP->flags & OPTF_HASLABEL )
        {
            findingP->detailP = allocPrintf("%04o carries the label %s, so control can arrive inside the pattern",
                entryP->addr, firstLabelName(entryP));
            return(OPTWHY_P1_LABEL);
        }

        if( entryP->flags & OPTF_JUMPTARGET )
        {
            findingP->detailP = allocPrintf("%04o is jumped to from elsewhere, so control can arrive inside the pattern",
                entryP->addr);
            return(OPTWHY_P1_ENTRY);
        }

        if( entryP->flags & OPTF_ENTRY )
        {
            findingP->detailP = allocPrintf("%04o is an entry point of the program, so control can arrive inside the pattern",
                entryP->addr);
            return(OPTWHY_P1_ENTRY);
        }
    }

    return(OPTWHY_NONE);
}

// Precondition P2: no word of the pattern may be written while the program
// runs, and no word's address may be used as a value.  Both apply to every
// word of the pattern, the first included: a word that is written is not the
// instruction the source spelled by the time it executes, and a word whose
// address is a value can be reached by an indirect jump or an xct that no
// analysis here can follow.
// Returns OPTWHY_NONE when the precondition holds, the reason otherwise.
static OptWhy
checkP2(OptFindingP findingP)
{
int i;
OptWordP entryP;
char evidence[OPTMSG_SIZE];

    for( i = 0; i < findingP->wordCount; ++i )
    {
        entryP = findingP->wordsP[i];

        if( entryP->flags & OPTF_PATCHED )
        {
            describeReferrer(entryP, OPTR_WRITE, 1, evidence, sizeof(evidence));
            findingP->detailP = allocPrintf("the address field of %04o is patched %s", entryP->addr, evidence);
            return(OPTWHY_P2_WRITTEN);
        }

        if( entryP->flags & OPTF_WRITTEN )
        {
            describeReferrer(entryP, OPTR_WRITE, 0, evidence, sizeof(evidence));
            findingP->detailP = allocPrintf("%04o is written %s", entryP->addr, evidence);
            return(OPTWHY_P2_WRITTEN);
        }

        if( entryP->flags & OPTF_TAKEN )
        {
            describeReferrer(entryP, OPTR_TAKEN, 0, evidence, sizeof(evidence));
            findingP->detailP = allocPrintf("the address %04o is used as a value %s", entryP->addr, evidence);
            return(OPTWHY_P2_TAKEN);
        }
    }

    return(OPTWHY_NONE);
}

// Precondition P3: no word of the pattern may be the target of an xct.  An
// xct runs one word in place, so the word has to stay one word and keep
// doing what it did.
// Returns OPTWHY_NONE when the precondition holds, the reason otherwise.
static OptWhy
checkP3(OptFindingP findingP)
{
int i;
OptWordP entryP;
char evidence[OPTMSG_SIZE];

    for( i = 0; i < findingP->wordCount; ++i )
    {
        entryP = findingP->wordsP[i];

        if( entryP->flags & OPTF_XCTTARGET )
        {
            describeReferrer(entryP, OPTR_EXECUTE, 0, evidence, sizeof(evidence));
            findingP->detailP = allocPrintf("%04o is executed in place %s", entryP->addr, evidence);
            return(OPTWHY_P3_XCT);
        }
    }

    return(OPTWHY_NONE);
}

// Precondition P4: no skip-class word immediately before the pattern.  If one
// is there, the pattern's first word may not execute at all, and a rewrite
// that changes how many words the pattern occupies changes which word the
// skip lands on.
//
// Task A4's OPTF_AFTERSKIP comes from optSkipWord(), which counts the skip
// group, isp, sad and sas.  It does not count div, and the handbook says the
// instruction after a div is skipped unless an overflow occurs, so div is
// added here.  The flag itself is left as task A4 accepted it; the omission
// is reported with this task rather than repaired inside another task's
// results.
// Returns OPTWHY_NONE when the precondition holds, the reason otherwise.
static OptWhy
checkP4(OptTableP tableP, OptFindingP findingP)
{
OptWordP firstP;
OptWordP prevP;
char spell[OPTMSG_SIZE];

    firstP = findingP->wordsP[0];
    prevP = wordAt(tableP, firstP->bank, (firstP->addr - 1));

    if( !(firstP->flags & OPTF_AFTERSKIP) && !isDivWord(prevP) )
    {
        return(OPTWHY_NONE);
    }

    if( prevP )
    {
        spellWord(prevP, spell, sizeof(spell));
        findingP->detailP = allocPrintf("%04o is the skip-class word %s, so %04o may not execute at all",
            prevP->addr, spell, firstP->addr);
    }
    else
    {
        findingP->detailP = allocPrintf("the word before %04o is a skip, so %04o may not execute at all",
            firstP->addr, firstP->addr);
    }

    return(OPTWHY_P4_SKIP);
}

// Run the shared preconditions a rule asks for, in their numbered order, and
// stop at the first that fails.
// Returns OPTWHY_NONE when they all hold, the first failure's reason
// otherwise; on a failure the finding's detail says which word and why.
static OptWhy
checkShared(OptTableP tableP, OptFindingP findingP, unsigned int which)
{
OptWhy why;

    if( which & OPTPC_P1 )
    {
        if( (why = checkP1(findingP)) != OPTWHY_NONE )
        {
            return(why);
        }
    }

    if( which & OPTPC_P2 )
    {
        if( (why = checkP2(findingP)) != OPTWHY_NONE )
        {
            return(why);
        }
    }

    if( which & OPTPC_P3 )
    {
        if( (why = checkP3(findingP)) != OPTWHY_NONE )
        {
            return(why);
        }
    }

    if( which & OPTPC_P4 )
    {
        if( (why = checkP4(tableP, findingP)) != OPTWHY_NONE )
        {
            return(why);
        }
    }

    return(OPTWHY_NONE);
}

// Is a word a div?  The handbook says the instruction after one is skipped
// unless an overflow occurs, which makes it skip-class for P4.
// Returns 1 for a div, 0 otherwise or for NILP.
static int
isDivWord(OptWordP entryP)
{
    return( isMemrefOp(entryP, 056) );
}

// Is a word a particular memory reference instruction, spelled directly?
// The indirect bit is deliberately not tested here: every caller that cares
// tests decode.memIndirect itself, and jda shares cal's opcode with the bit
// set, so a blanket test would be wrong for one of the two.
// Returns 1 when it matches, 0 otherwise or for NILP.
static int
isMemrefOp(OptWordP entryP, int opcode)
{
    if( !entryP )
    {
        return(0);
    }

    if( entryP->decode.group != OPTG_MEMREF )
    {
        return(0);
    }

    return( entryP->decode.opcode == opcode );
}

// ---------------------------------------------------------------------------
// The rules.
// ---------------------------------------------------------------------------

// Does T6 own this pair of words?  T6 and T1 would both fire on a bare cla
// in front of an operate word that clears AC itself, and would suggest the
// same single word; T6's message is the useful one ("the cla is redundant"),
// so it takes the pair and T1 leaves it alone.  The first word must be a
// bare cla and nothing else: any other micro-op in it has to survive.
//
// lat and lap OR into AC, so a cla in front of one is redundant only when the
// word carries the cla bit itself -- which is exactly how permsyms.def spells
// them (762200 and 760300), the handbook noting they are "usually combined"
// with clear accumulator.  A lap pair is owned here and then refused inside
// ruleT6(), because lap's value moves with the word; owning it is what makes
// the refusal appear in the report instead of vanishing.
// Returns 1 when T6 owns the pair, 0 otherwise.
static int
t6Claims(OptWordP w1P, OptWordP w2P)
{
    if( !w1P || !w2P )
    {
        return(0);
    }

    if( (w1P->decode.group != OPTG_OPERATE) || (w1P->decode.microBits != OPTM_CLA) )
    {
        return(0);
    }

    if( w2P->decode.group == OPTG_LAW )
    {
        return(1);
    }

    if( isMemrefOp(w2P, 020) )      // lac, indirect or not: it fills AC either way
    {
        return(1);
    }

    if( (w2P->decode.group == OPTG_OPERATE)
        && (w2P->decode.microBits & OPTM_CLA)
        && (w2P->decode.microBits & (OPTM_LAT | OPTM_LAP)) )
    {
        return(1);
    }

    return(0);
}

// Does T7 own this pair?  A bare cli in front of a lio, which fills IO
// outright.  lio is a memory reference, so this never collides with T1.
// Returns 1 when T7 owns the pair, 0 otherwise.
static int
t7Claims(OptWordP w1P, OptWordP w2P)
{
    if( !w1P || !w2P )
    {
        return(0);
    }

    if( (w1P->decode.group != OPTG_OPERATE) || (w1P->decode.microBits != OPTM_CLI) )
    {
        return(0);
    }

    return( isMemrefOp(w2P, 022) );
}

// May two consecutive operate words be merged?  Everything here is about the
// words' contents; the shared preconditions are about their surroundings and
// are checked separately.
//
// The phase table (task A2, from the handbook page 21 and the operate timing
// note of 08-Sep-2026) says when the hardware applies each micro-op.  One
// merged word applies them all in that order; two words apply the first
// word's set completely before the second word's.  The two agree only when
// nothing in the first word acts later than anything in the second.  Four
// things the phase order alone does not catch are tested first.
// Returns OPTWHY_NONE when the merge is sound, the reason otherwise.
static OptWhy
t1Refusal(OptWordP w1P, OptWordP w2P, OptFindingP findingP)
{
int i;
int j;
unsigned int m1;
unsigned int m2;
const OptMicroOp *aP;
const OptMicroOp *bP;

    m1 = w1P->decode.microBits;
    m2 = w2P->decode.microBits;

    // lap ORs into AC the address of the word after the one it is in.  The
    // merge moves the second word onto the first word's address, so a lap
    // there would read a program counter one lower.  A lap in the first word
    // does not move and is fine.
    if( optMicroOpPresent(m2, OPTMO_LAP) )
    {
        findingP->detailP = allocPrintf("the second word carries lap, whose value is its own address plus one; merging moves it one word lower");
        return(OPTWHY_LAP);
    }

    // lia and lai are armed at TP8 and complete during the next instruction's
    // fetch, so in separate words they are two copies, the first finished
    // before the second is armed.  In one word they are a single exchange.
    if( (optMicroOpPresent(m1, OPTMO_LIA) && optMicroOpPresent(m2, OPTMO_LAI))
        || (optMicroOpPresent(m1, OPTMO_LAI) && optMicroOpPresent(m2, OPTMO_LIA)) )
    {
        findingP->detailP = allocPrintf("lia in one word and lai in the other are two copies; in a single word they are one exchange");
        return(OPTWHY_EXCHANGE);
    }

    // A micro-op that is not idempotent cannot be folded into one copy of
    // itself: two halts are two stops, and two complements cancel.
    for( i = 0; i < (int)(sizeof(optNotIdempotent) / sizeof(optNotIdempotent[0])); ++i )
    {
        if( optMicroOpPresent(m1, optNotIdempotent[i]) && optMicroOpPresent(m2, optNotIdempotent[i]) )
        {
            findingP->detailP = allocPrintf("both words carry %s, which done twice is not the same as done once",
                optMicroOps[optNotIdempotent[i]].nameP);
            return(OPTWHY_REPEAT);
        }
    }

    // The flag field is one four bit slot: a word cannot clear one flag and
    // set another, nor name two flag numbers.
    if( (w1P->decode.flagOp != OPTFLAG_NONE) && (w2P->decode.flagOp != OPTFLAG_NONE) )
    {
        if( (w1P->decode.flagOp != w2P->decode.flagOp) || (w1P->decode.flagNum != w2P->decode.flagNum) )
        {
            findingP->detailP = allocPrintf("the words carry %s %o and %s %o, and one word has only one flag field",
                (w1P->decode.flagOp == OPTFLAG_CLF)?"clf":"stf", w1P->decode.flagNum,
                (w2P->decode.flagOp == OPTFLAG_CLF)?"clf":"stf", w2P->decode.flagNum);
            return(OPTWHY_FLAGS);
        }
    }

    // Now the phase order itself.
    for( i = 0; i < OPTMO_COUNT; ++i )
    {
        if( !optMicroOpPresent(m1, (OptMicroId)i) )
        {
            continue;
        }

        aP = &optMicroOps[i];

        for( j = 0; j < OPTMO_COUNT; ++j )
        {
            if( i == j )
            {
                continue;
            }

            if( !optMicroOpPresent(m2, (OptMicroId)j) )
            {
                continue;
            }

            bP = &optMicroOps[j];

            if( (aP->phase > bP->phase) || ((aP->phase == bP->phase) && (aP->order > bP->order)) )
            {
                findingP->detailP = allocPrintf("%s in the first word acts after %s in the second, and one word would run them the other way round",
                    aP->nameP, bP->nameP);
                return(OPTWHY_PHASE);
            }
        }
    }

    return(OPTWHY_NONE);
}

// T1: two consecutive operate words become one with the union of their
// micro-ops (study section 6).  Saves the word and its cycle.
static void
ruleT1(OptTableP tableP, int bank, int addr)
{
OptWordP wordsP[2];
OptFindingP findingP;
OptWhy why;
unsigned int merged;
char spell[OPTMSG_SIZE];

    if( !(wordsP[0] = patternWord(tableP, bank, addr)) )
    {
        return;
    }

    if( wordsP[0]->decode.group != OPTG_OPERATE )
    {
        return;
    }

    if( !(wordsP[1] = patternWord(tableP, bank, (addr + 1))) )
    {
        return;
    }

    if( wordsP[1]->decode.group != OPTG_OPERATE )
    {
        return;
    }

    if( t6Claims(wordsP[0], wordsP[1]) )
    {
        return;
    }

    findingP = startFinding(OPTRULE_T1, wordsP, 2);
    merged = (wordsP[0]->decode.microBits | wordsP[1]->decode.microBits);
    spellOperate(merged, spell, sizeof(spell));
    findingP->replaceP = allocPrintf("%s", spell);

    if( (why = t1Refusal(wordsP[0], wordsP[1], findingP)) != OPTWHY_NONE )
    {
        emitFinding(tableP, findingP, why);
        return;
    }

    if( (why = checkShared(tableP, findingP, OPTPC_ALL)) != OPTWHY_NONE )
    {
        emitFinding(tableP, findingP, why);
        return;
    }

    findingP->saveWords = 1;
    findingP->saveTime = OPTTIME_CYCLE;
    findingP->detailP = allocPrintf("the merged word is %06o", (0760000 | merged));
    emitFinding(tableP, findingP, OPTWHY_NONE);
}

// T1b: two consecutive shifts of the same opcode become one shift of the
// summed count, if the sum is the nine steps a shift word can name.  The
// count is the number of one bits in bits 9 to 17, and am1 spells it with
// the 1s to 9s symbols.
static void
ruleT1b(OptTableP tableP, int bank, int addr)
{
OptWordP wordsP[2];
OptFindingP findingP;
OptWhy why;
int total;

    if( !(wordsP[0] = patternWord(tableP, bank, addr)) )
    {
        return;
    }

    if( wordsP[0]->decode.group != OPTG_SHIFT )
    {
        return;
    }

    if( !(wordsP[1] = patternWord(tableP, bank, (addr + 1))) )
    {
        return;
    }

    if( wordsP[1]->decode.group != OPTG_SHIFT )
    {
        return;
    }

    // Bits 0 to 8 are the whole opcode: the group, the direction, shift
    // against rotate, and which registers.  Two words agree on all of it or
    // they are different instructions.
    if( (wordsP[0]->value & ~OPTSH_COUNT) != (wordsP[1]->value & ~OPTSH_COUNT) )
    {
        return;
    }

    // A shift word that names no register does nothing; there is no useful
    // suggestion to make about a pair of them.
    if( !wordsP[0]->decode.shiftRegs )
    {
        return;
    }

    findingP = startFinding(OPTRULE_T1B, wordsP, 2);
    total = (wordsP[0]->decode.shiftCount + wordsP[1]->decode.shiftCount);

    if( total > 9 )
    {
        // No replacement is offered: am1's shift counts are the symbols 1s to
        // 9s and there is no tenth, so there is no word to suggest.
        findingP->detailP = allocPrintf("%d steps and %d steps make %d, and bits 9 to 17 hold at most nine",
            wordsP[0]->decode.shiftCount, wordsP[1]->decode.shiftCount, total);
        emitFinding(tableP, findingP, OPTWHY_SHIFTCOUNT);
        return;
    }

    findingP->replaceP = allocPrintf("%s %ds", wordsP[0]->decode.mnemonicP, total);

    if( (why = checkShared(tableP, findingP, OPTPC_ALL)) != OPTWHY_NONE )
    {
        emitFinding(tableP, findingP, why);
        return;
    }

    findingP->saveWords = 1;
    findingP->saveTime = OPTTIME_CYCLE;
    findingP->detailP = allocPrintf("the merged word is %06o", ((wordsP[0]->value & ~OPTSH_COUNT) | ((1 << total) - 1)));
    emitFinding(tableP, findingP, OPTWHY_NONE);
}

// Can a skip-class word be written the other way round, and what would the
// word be?  The skip group's bit 5 reverses the whole combined condition
// (handbook page 20: sza with bit 5 set "becomes Do Not Skip on Zero
// Accumulator").  sad and sas are each other's inverse and differ by one bit
// of the instruction code.  isp and div also skip and have no inverted form.
// Returns 1 and the inverted word in *valueP, or 0 when there is no inverse.
static int
invertSkip(OptWordP entryP, int *valueP)
{
    if( !entryP )
    {
        return(0);
    }

    if( entryP->decode.group == OPTG_SKIP )
    {
        *valueP = (entryP->value ^ 0010000);
        return(1);
    }

    if( entryP->decode.group == OPTG_MEMREF )
    {
        switch( entryP->decode.opcode )
        {
        case 050:       // sad, skip if AC and Y differ
        case 052:       // sas, skip if AC and Y are the same
            *valueP = (entryP->value ^ 0020000);
            return(1);

        default:
            break;
        }
    }

    return(0);
}

// T2: "skip; jmp .+2; W" is the inverted skip followed by W.  The jmp's
// target is its own address plus two, which is the word after W, so the two
// forms take the same path either way the condition falls.  Saves the jmp
// word, and its cycle on the path that used to take it.
static void
ruleT2(OptTableP tableP, int bank, int addr)
{
OptWordP wordsP[3];
OptFindingP findingP;
OptWhy why;
int inverted;
char spell[OPTMSG_SIZE];
char operand[OPTMSG_SIZE];

    if( (addr + 3) >= BANKSIZE )
    {
        return;
    }

    if( !(wordsP[0] = patternWord(tableP, bank, addr)) )
    {
        return;
    }

    if( !optSkipWord(wordsP[0]) && !isDivWord(wordsP[0]) )
    {
        return;
    }

    if( !(wordsP[1] = patternWord(tableP, bank, (addr + 1))) )
    {
        return;
    }

    if( !isMemrefOp(wordsP[1], 060) || wordsP[1]->decode.memIndirect )
    {
        return;
    }

    if( wordsP[1]->decode.address != (addr + 3) )
    {
        return;
    }

    if( !(wordsP[2] = patternWord(tableP, bank, (addr + 2))) )
    {
        return;
    }

    findingP = startFinding(OPTRULE_T2, wordsP, 3);

    if( !invertSkip(wordsP[0], &inverted) )
    {
        spellWord(wordsP[0], spell, sizeof(spell));
        findingP->detailP = allocPrintf("%s cannot be written the other way round: it is not the skip group and it is not sad or sas",
            spell);
        emitFinding(tableP, findingP, OPTWHY_NOINVERSE);
        return;
    }

    if( wordsP[0]->decode.group == OPTG_SKIP )
    {
        spellSkipValue(inverted, spell, sizeof(spell));
        findingP->replaceP = allocPrintf("%s", spell);
    }
    else
    {
        spellOperand(wordsP[0], operand, sizeof(operand));
        findingP->replaceP = allocPrintf("%s%s %s", (wordsP[0]->decode.opcode == 050)?"sas":"sad",
            (wordsP[0]->decode.memIndirect)?" i":"", operand);
    }

    if( (why = checkShared(tableP, findingP, OPTPC_ALL)) != OPTWHY_NONE )
    {
        emitFinding(tableP, findingP, why);
        return;
    }

    findingP->saveWords = 1;
    findingP->saveTime = OPTTIME_CYCLE;
    findingP->detailP = allocPrintf("drop the jmp at %04o and reverse the skip; the word at %04o stays where it is in the source",
        wordsP[1]->addr, wordsP[2]->addr);
    emitFinding(tableP, findingP, OPTWHY_NONE);
}

// T3: a jmp whose target is itself a jmp can name the far target directly.
// Layout neutral: it removes no word, only the cycle spent on the hop, and
// only on the executions that take it.  If the intermediate word is later
// patched the source jump now bypasses it, which is exactly why the rule
// refuses a patched intermediate.
static void
ruleT3(OptTableP tableP, int bank, int addr)
{
OptWordP wordsP[1];
OptWordP viaP;
OptFindingP findingP;
OptWhy why;
char evidence[OPTMSG_SIZE];
char operand[OPTMSG_SIZE];

    if( !(wordsP[0] = patternWord(tableP, bank, addr)) )
    {
        return;
    }

    if( !isMemrefOp(wordsP[0], 060) || wordsP[0]->decode.memIndirect )
    {
        return;
    }

    if( wordsP[0]->decode.address == addr )
    {
        return;     // a jmp to itself: a deliberate spin, not a hop
    }

    if( !(viaP = patternWord(tableP, bank, wordsP[0]->decode.address)) )
    {
        return;
    }

    if( !isMemrefOp(viaP, 060) || viaP->decode.memIndirect )
    {
        return;
    }

    if( viaP->decode.address == viaP->addr )
    {
        return;     // the intermediate spins on itself: following it is not a hop
    }

    if( viaP->decode.address == wordsP[0]->decode.address )
    {
        return;     // nothing would change
    }

    findingP = startFinding(OPTRULE_T3, wordsP, 1);

    // No suggested line is offered for the two refusals below, unlike every
    // other refusal in this file.  Elsewhere the naive rewrite is a real word
    // and showing it is what the reason line is about; here the far target is
    // whatever the last write to the intermediate put there, so the address
    // this word happens to hold at assembly time (usually 0, from a "jmp 0"
    // placeholder) would print as a destination the rule never meant.
    if( viaP->flags & OPTF_PATCHED )
    {
        describeReferrer(viaP, OPTR_WRITE, 1, evidence, sizeof(evidence));
        findingP->detailP = allocPrintf("the intermediate jmp at %04o is patched %s, so where it goes is not fixed",
            viaP->addr, evidence);
        emitFinding(tableP, findingP, OPTWHY_PATCHED);
        return;
    }

    if( viaP->flags & OPTF_WRITTEN )
    {
        describeReferrer(viaP, OPTR_WRITE, 0, evidence, sizeof(evidence));
        findingP->detailP = allocPrintf("the intermediate jmp at %04o is written %s, so it is not always a jmp",
            viaP->addr, evidence);
        emitFinding(tableP, findingP, OPTWHY_P2_WRITTEN);
        return;
    }

    // Past the two refusals above the intermediate's address field is fixed,
    // so the far target it names is the one the rewrite would jump to.
    spellOperand(viaP, operand, sizeof(operand));
    findingP->replaceP = allocPrintf("jmp %s", operand);

    // The rewritten word stays one word in the same place, so only the two
    // preconditions about the word being what it appears to be apply.
    if( (why = checkShared(tableP, findingP, OPTPC_P2)) != OPTWHY_NONE )
    {
        emitFinding(tableP, findingP, why);
        return;
    }

    findingP->saveWords = 0;
    findingP->saveTime = OPTTIME_CYCLE;
    findingP->perHop = 1;
    findingP->detailP = allocPrintf("%04o holds a jmp to %04o; the word at %04o is unchanged and any other route through it still works",
        viaP->addr, viaP->decode.address, viaP->addr);
    emitFinding(tableP, findingP, OPTWHY_NONE);
}

// T6: a bare cla in front of an instruction that fills AC outright.  The cla
// goes; the loading instruction moves up one word.
static void
ruleT6(OptTableP tableP, int bank, int addr)
{
OptWordP wordsP[2];
OptFindingP findingP;
OptWhy why;
char spell[OPTMSG_SIZE];

    if( !(wordsP[0] = patternWord(tableP, bank, addr)) )
    {
        return;
    }

    if( !(wordsP[1] = patternWord(tableP, bank, (addr + 1))) )
    {
        return;
    }

    if( !t6Claims(wordsP[0], wordsP[1]) )
    {
        return;
    }

    findingP = startFinding(OPTRULE_T6, wordsP, 2);
    spellWord(wordsP[1], spell, sizeof(spell));
    findingP->replaceP = allocPrintf("%s", spell);

    // The catalogue lists lap among the instructions a cla in front of is
    // redundant, and for the accumulator it is: permsyms.def spells lap as
    // 760300, which carries the cla bit.  But lap ORs in the address of the
    // word AFTER the one it sits in, and dropping the cla moves it one word
    // lower, so the value it loads changes.  The pattern is reported and
    // refused rather than quietly not looked for.
    if( (wordsP[1]->decode.group == OPTG_OPERATE) && (wordsP[1]->decode.microBits & OPTM_LAP) )
    {
        findingP->detailP = allocPrintf("lap loads its own address plus one, and dropping the cla at %04o would move it one word lower",
            wordsP[0]->addr);
        emitFinding(tableP, findingP, OPTWHY_LAP);
        return;
    }

    if( (why = checkShared(tableP, findingP, OPTPC_ALL)) != OPTWHY_NONE )
    {
        emitFinding(tableP, findingP, why);
        return;
    }

    findingP->saveWords = 1;
    findingP->saveTime = OPTTIME_CYCLE;
    findingP->detailP = allocPrintf("%s fills AC on its own, so the cla at %04o changes nothing",
        spell, wordsP[0]->addr);
    emitFinding(tableP, findingP, OPTWHY_NONE);
}

// T7: a bare cli in front of a lio, which fills IO outright.
static void
ruleT7(OptTableP tableP, int bank, int addr)
{
OptWordP wordsP[2];
OptFindingP findingP;
OptWhy why;
char spell[OPTMSG_SIZE];

    if( !(wordsP[0] = patternWord(tableP, bank, addr)) )
    {
        return;
    }

    if( !(wordsP[1] = patternWord(tableP, bank, (addr + 1))) )
    {
        return;
    }

    if( !t7Claims(wordsP[0], wordsP[1]) )
    {
        return;
    }

    findingP = startFinding(OPTRULE_T7, wordsP, 2);
    spellWord(wordsP[1], spell, sizeof(spell));
    findingP->replaceP = allocPrintf("%s", spell);

    if( (why = checkShared(tableP, findingP, OPTPC_ALL)) != OPTWHY_NONE )
    {
        emitFinding(tableP, findingP, why);
        return;
    }

    findingP->saveWords = 1;
    findingP->saveTime = OPTTIME_CYCLE;
    findingP->detailP = allocPrintf("%s fills IO on its own, so the cli at %04o changes nothing",
        spell, wordsP[0]->addr);
    emitFinding(tableP, findingP, OPTWHY_NONE);
}

// T8: a lac or lio of a constant pool word whose value an augmented
// instruction can produce without touching memory.  Layout neutral -- the
// word stays where it is -- but it drops from two cycles to one, and if
// nothing else names the pool word the pool loses a word too.
//
// The pool is the only place this fires.  A lac of an ordinary variable that
// happens to hold zero is a different thing: the variable can be written,
// and then the lac is not loading zero any more.
static void
ruleT8(OptTableP tableP, int bank, int addr)
{
OptWordP wordsP[1];
OptWordP poolP;
OptFindingP findingP;
OptRuleId rule;
OptWhy why;
OptEdgeP edgeP;
int value;
int others;
char evidence[OPTMSG_SIZE];
char replace[OPTMSG_SIZE];

    if( !(wordsP[0] = patternWord(tableP, bank, addr)) )
    {
        return;
    }

    if( wordsP[0]->decode.memIndirect )
    {
        return;
    }

    if( !isMemrefOp(wordsP[0], 020) && !isMemrefOp(wordsP[0], 022) )
    {
        return;
    }

    if( !(poolP = wordAt(tableP, bank, wordsP[0]->decode.address)) )
    {
        return;
    }

    if( (poolP->kind != OPTK_CONST) || (poolP->flags & (OPTF_RESERVED | OPTF_DUPADDR)) )
    {
        return;
    }

    value = poolP->value;
    rule = OPTRULE_COUNT;

    if( isMemrefOp(wordsP[0], 022) )
    {
        // lio: the only augmented instruction that fills IO is cli.
        if( value != 0 )
        {
            return;
        }

        rule = OPTRULE_T8A;
        strcpy(replace, "cli");
    }
    else if( !value )
    {
        rule = OPTRULE_T8B;
        strcpy(replace, "cla");
    }
    else if( value == WRDMASK )
    {
        rule = OPTRULE_T8C;
        strcpy(replace, "cla cma");
    }
    else if( value <= ADDRMASK )
    {
        rule = OPTRULE_T8D;
        sprintf(replace, "law 0o%o", value);
    }
    else if( ((~value) & WRDMASK) <= ADDRMASK )
    {
        rule = OPTRULE_T8D;
        sprintf(replace, "law i 0o%o", ((~value) & WRDMASK));
    }
    else
    {
        return;
    }

    findingP = startFinding(rule, wordsP, 1);
    findingP->replaceP = allocPrintf("%s", replace);

    if( poolP->flags & OPTF_WRITTEN )
    {
        describeReferrer(poolP, OPTR_WRITE, 0, evidence, sizeof(evidence));
        findingP->detailP = allocPrintf("the pool word at %04o is written %s, so it does not always hold %06o",
            poolP->addr, evidence, value);
        emitFinding(tableP, findingP, OPTWHY_P2_WRITTEN);
        return;
    }

    if( wordsP[0]->flags & OPTF_PATCHED )
    {
        describeReferrer(wordsP[0], OPTR_WRITE, 1, evidence, sizeof(evidence));
        findingP->detailP = allocPrintf("the address field of %04o is patched %s, so it does not always name the pool word",
            wordsP[0]->addr, evidence);
        emitFinding(tableP, findingP, OPTWHY_PATCHED);
        return;
    }

    // The word is rewritten where it stands, so only P2 applies.
    if( (why = checkShared(tableP, findingP, OPTPC_P2)) != OPTWHY_NONE )
    {
        emitFinding(tableP, findingP, why);
        return;
    }

    others = 0;

    for( edgeP = poolP->inP; edgeP; edgeP = edgeP->nextInP )
    {
        if( edgeP->fromP != wordsP[0] )
        {
            ++others;
        }
    }

    findingP->saveTime = (OPTTIME_MEMREF - OPTTIME_CYCLE);

    if( !others )
    {
        findingP->saveTemps = 1;
        findingP->detailP = allocPrintf("the pool word at %04o holds %06o and nothing else names it, so the pool loses a word too",
            poolP->addr, value);
    }
    else
    {
        findingP->detailP = allocPrintf("the pool word at %04o holds %06o and %d other reference%s name%s it, so it stays",
            poolP->addr, value, others, (others == 1)?"":"s", (others == 1)?"s":"");
    }

    emitFinding(tableP, findingP, OPTWHY_NONE);
}

// T13: a jmp to the very next word.  Control goes there anyway.
static void
ruleT13(OptTableP tableP, int bank, int addr)
{
OptWordP wordsP[1];
OptFindingP findingP;
OptWhy why;

    if( !(wordsP[0] = patternWord(tableP, bank, addr)) )
    {
        return;
    }

    if( !isMemrefOp(wordsP[0], 060) || wordsP[0]->decode.memIndirect )
    {
        return;
    }

    if( wordsP[0]->decode.address != (addr + 1) )
    {
        return;
    }

    findingP = startFinding(OPTRULE_T13, wordsP, 1);
    findingP->replaceP = allocPrintf("(delete the word)");

    // P1 has nothing to look at in a one word pattern, but the other three
    // matter: a patched word is not this jmp when it runs, an xct of it goes
    // to the word after the jmp and not the word after the xct, and a skip in
    // front of it reaches past the following word once the jmp is gone.
    if( (why = checkShared(tableP, findingP, (OPTPC_P2 | OPTPC_P3 | OPTPC_P4))) != OPTWHY_NONE )
    {
        emitFinding(tableP, findingP, why);
        return;
    }

    findingP->saveWords = 1;
    findingP->saveTime = OPTTIME_CYCLE;
    findingP->detailP = allocPrintf("%04o is the next word, which the machine would reach without the jmp", (addr + 1));
    emitFinding(tableP, findingP, OPTWHY_NONE);
}

// Is a temporary private to a pattern?  The rewrite T14 suggests stops
// writing the temporary altogether, so anything else that reads it, writes
// it, or uses its address would see a different program.
// Returns 1 when nothing outside the pattern reaches it, 0 otherwise; on a 0
// the finding's detail says what does.
static int
tempIsPrivate(OptTableP tableP, OptFindingP findingP, OptWordP tempP)
{
OptEdgeP edgeP;
int i;
int mine;
char spell[OPTMSG_SIZE];

    (void)tableP;

    if( tempP->flags & (OPTF_TAKEN | OPTF_XCTTARGET | OPTF_CODE) )
    {
        findingP->detailP = allocPrintf("the temporary at %04o is %s, so it is more than a scratch word",
            tempP->addr,
            (tempP->flags & OPTF_TAKEN)?"an address something uses as a value":
            ((tempP->flags & OPTF_XCTTARGET)?"executed by an xct":"classified code"));
        return(0);
    }

    for( edgeP = tempP->inP; edgeP; edgeP = edgeP->nextInP )
    {
        mine = 0;

        for( i = 0; i < findingP->wordCount; ++i )
        {
            if( edgeP->fromP == findingP->wordsP[i] )
            {
                mine = 1;
                break;
            }
        }

        if( mine )
        {
            continue;
        }

        if( edgeP->fromP )
        {
            spellWord(edgeP->fromP, spell, sizeof(spell));
            findingP->detailP = allocPrintf("the temporary at %04o is also reached by the %s at bank %d %04o, line %d",
                tempP->addr, spell, edgeP->fromP->bank, edgeP->fromP->addr, edgeP->fromP->lineNo);
        }
        else
        {
            findingP->detailP = allocPrintf("the temporary at %04o is reached from outside the pattern", tempP->addr);
        }

        return(0);
    }

    return(1);
}

// T14: a copy between AC and IO made through a temporary, which the PDP-1D
// does in one word.  "dac t; lio t" leaves IO holding AC, which is lia; "dio
// t; lac t" leaves AC holding IO, which is lai; and the four word form that
// saves both registers and reloads them crosswise is the exchange, swp.
// (Directions from Docs/UsingPDP-1DInstructions.md: lia 760020 "load IO from
// AC", lai 760040 "load AC from IO", lsw and swp 760060 "swap AC and IO".)
//
// The exchange is checked first and marks the addresses it took, so the two
// word forms do not also report the pair inside it.  The four orderings of
// the exchange are every one the machine allows: both deposits have to
// happen before either load, because each load overwrites a register the
// other deposit reads.
static void
ruleT14(OptTableP tableP, int bank, int addr, int *claimedToP)
{
OptWordP wordsP[OPTFIND_MAXWORDS];
OptWordP depositP[2];
OptWordP loadP[2];
OptWordP tempAcP;
OptWordP tempIoP;
OptFindingP findingP;
OptWhy why;
int i;
int acAddr;
int ioAddr;

    if( addr <= *claimedToP )
    {
        return;
    }

    // --- the four word exchange -------------------------------------------

    if( (addr + 3) < BANKSIZE )
    {
        for( i = 0; i < 4; ++i )
        {
            wordsP[i] = patternWord(tableP, bank, (addr + i));
        }

        if( wordsP[0] && wordsP[1] && wordsP[2] && wordsP[3]
            && !wordsP[0]->decode.memIndirect && !wordsP[1]->decode.memIndirect
            && !wordsP[2]->decode.memIndirect && !wordsP[3]->decode.memIndirect )
        {
            depositP[0] = NILP;
            depositP[1] = NILP;
            loadP[0] = NILP;
            loadP[1] = NILP;

            for( i = 0; i < 2; ++i )
            {
                if( isMemrefOp(wordsP[i], 024) )        // dac: AC to the temporary
                {
                    depositP[0] = wordsP[i];
                }
                else if( isMemrefOp(wordsP[i], 032) )   // dio: IO to the temporary
                {
                    depositP[1] = wordsP[i];
                }
            }

            for( i = 2; i < 4; ++i )
            {
                if( isMemrefOp(wordsP[i], 020) )        // lac: AC from a temporary
                {
                    loadP[0] = wordsP[i];
                }
                else if( isMemrefOp(wordsP[i], 022) )   // lio: IO from a temporary
                {
                    loadP[1] = wordsP[i];
                }
            }

            if( depositP[0] && depositP[1] && loadP[0] && loadP[1] )
            {
                acAddr = depositP[0]->decode.address;   // where AC was put
                ioAddr = depositP[1]->decode.address;   // where IO was put

                // The exchange is the crosswise reload: AC comes back from
                // where IO went and IO from where AC went, through two
                // different words.
                if( (acAddr != ioAddr) && (loadP[0]->decode.address == ioAddr)
                    && (loadP[1]->decode.address == acAddr) )
                {
                    // The shape is an exchange whatever comes of it, so the
                    // two word forms must not also report the pair inside it,
                    // whether this finding ends up live or refused.
                    *claimedToP = (addr + 3);
                    findingP = startFinding(OPTRULE_T14, wordsP, 4);
                    findingP->replaceP = allocPrintf("swp");
                    tempAcP = wordAt(tableP, bank, acAddr);
                    tempIoP = wordAt(tableP, bank, ioAddr);

                    if( !tempAcP || !tempIoP )
                    {
                        findingP->detailP = allocPrintf("one of the temporaries at %04o and %04o has no emitted word",
                            acAddr, ioAddr);
                        emitFinding(tableP, findingP, OPTWHY_TEMPUSED);
                        return;
                    }

                    if( !tempIsPrivate(tableP, findingP, tempAcP) || !tempIsPrivate(tableP, findingP, tempIoP) )
                    {
                        emitFinding(tableP, findingP, OPTWHY_TEMPUSED);
                        return;
                    }

                    if( (why = checkShared(tableP, findingP, OPTPC_ALL)) != OPTWHY_NONE )
                    {
                        emitFinding(tableP, findingP, why);
                        return;
                    }

                    findingP->saveWords = 3;
                    findingP->saveTemps = 2;
                    findingP->saveTime = ((4 * OPTTIME_MEMREF) - OPTTIME_CYCLE);
                    findingP->detailP = allocPrintf("swp exchanges AC and IO in one word; the temporaries at %04o and %04o become free",
                        acAddr, ioAddr);
                    emitFinding(tableP, findingP, OPTWHY_NONE);
                    return;
                }
            }
        }
    }

    // --- the two word copies ----------------------------------------------

    if( !(wordsP[0] = patternWord(tableP, bank, addr)) )
    {
        return;
    }

    if( !(wordsP[1] = patternWord(tableP, bank, (addr + 1))) )
    {
        return;
    }

    if( wordsP[0]->decode.memIndirect || wordsP[1]->decode.memIndirect )
    {
        return;
    }

    if( wordsP[0]->decode.address != wordsP[1]->decode.address )
    {
        return;
    }

    if( isMemrefOp(wordsP[0], 024) && isMemrefOp(wordsP[1], 022) )
    {
        // dac t; lio t: IO ends up holding AC, and AC is untouched.
        findingP = startFinding(OPTRULE_T14, wordsP, 2);
        findingP->replaceP = allocPrintf("lia");
    }
    else if( isMemrefOp(wordsP[0], 032) && isMemrefOp(wordsP[1], 020) )
    {
        // dio t; lac t: AC ends up holding IO, and IO is untouched.
        findingP = startFinding(OPTRULE_T14, wordsP, 2);
        findingP->replaceP = allocPrintf("lai");
    }
    else
    {
        return;
    }

    if( !(tempAcP = wordAt(tableP, bank, wordsP[0]->decode.address)) )
    {
        findingP->detailP = allocPrintf("the temporary at %04o has no emitted word", wordsP[0]->decode.address);
        emitFinding(tableP, findingP, OPTWHY_TEMPUSED);
        return;
    }

    if( !tempIsPrivate(tableP, findingP, tempAcP) )
    {
        emitFinding(tableP, findingP, OPTWHY_TEMPUSED);
        return;
    }

    if( (why = checkShared(tableP, findingP, OPTPC_ALL)) != OPTWHY_NONE )
    {
        emitFinding(tableP, findingP, why);
        return;
    }

    findingP->saveWords = 1;
    findingP->saveTemps = 1;
    findingP->saveTime = ((2 * OPTTIME_MEMREF) - OPTTIME_CYCLE);
    findingP->detailP = allocPrintf("the temporary at %04o is written and read by nothing else, so it becomes free too",
        tempAcP->addr);
    emitFinding(tableP, findingP, OPTWHY_NONE);
}

// ---------------------------------------------------------------------------
// The report and the dump.
// ---------------------------------------------------------------------------

// Print one finding: where it is, what it would save or why it was refused,
// the words it names in octal and as the source spelled them, the suggested
// replacement, and the rule's note or the evidence for the refusal.
static void
printFinding(FILE *fP, OptFindingP findingP, int suppressed)
{
int i;
OptWordP entryP;
char spell[OPTMSG_SIZE];

    entryP = findingP->wordsP[0];

    fprintf(fP, "    bank %2d %04o  %s:%d", findingP->bank, findingP->addr,
        (entryP->fileP)?entryP->fileP:"-", entryP->lineNo);

    if( suppressed )
    {
        fprintf(fP, "  %s\n", optWhyTable[findingP->why].textP);
    }
    else
    {
        fprintf(fP, "  saves %d word%s", findingP->saveWords, (findingP->saveWords == 1)?"":"s");

        if( findingP->saveTemps )
        {
            fprintf(fP, ", %d storage word%s", findingP->saveTemps, (findingP->saveTemps == 1)?"":"s");
        }

        fprintf(fP, ", %d us%s\n", findingP->saveTime, (findingP->perHop)?" per hop taken":"");
    }

    for( i = 0; i < findingP->wordCount; ++i )
    {
        entryP = findingP->wordsP[i];
        spellWord(entryP, spell, sizeof(spell));
        fprintf(fP, "        %04o %06o  %s\n", entryP->addr, entryP->value, spell);
    }

    if( findingP->replaceP )
    {
        // A refused finding still shows the word a naive rewrite would give:
        // it is what the reason line is about.  "would become" rather than
        // "becomes" so that nobody types it in.
        fprintf(fP, "        %s %s\n", (suppressed)?"would become:":"becomes:", findingP->replaceP);
    }

    if( findingP->detailP )
    {
        fprintf(fP, "        %s %s\n", (suppressed)?"because":"note:", findingP->detailP);
    }
}

// The header notes: what the reader is holding, what the preconditions mean,
// and the one precondition the analysis cannot check for them.
static void
writeHeaderNotes(FILE *fP)
{
    fprintf(fP, "\nThis is an advisory report.  Nothing in the assembled program was changed;\n");
    fprintf(fP, "every suggestion below is a source change for a person to make and test.\n");
    fprintf(fP, "Addresses and word values are octal.  A number inside a suggested source\n");
    fprintf(fP, "line carries an explicit 0o prefix, because a bare number in am1 is read in\n");
    fprintf(fP, "whatever radix is current where it is pasted.\n");
    fprintf(fP, "\nA suggestion is made only when all of these hold (study section 6):\n");
    fprintf(fP, "  P1  no label, jump target or program entry inside the pattern, so nothing\n");
    fprintf(fP, "      can arrive anywhere but at its first word.\n");
    fprintf(fP, "  P2  no word of the pattern is written or patched while the program runs,\n");
    fprintf(fP, "      and no word's address is used as a value.\n");
    fprintf(fP, "  P3  no word of the pattern is executed in place by an xct.\n");
    fprintf(fP, "  P4  no skip-class word immediately before the pattern.\n");
    fprintf(fP, "  P5  no location involved is shared with a device, a sequence break handler,\n");
    fprintf(fP, "      the drum, DCS2 or a high speed channel.\n");
    fprintf(fP, "\nP5 CANNOT BE CHECKED HERE and is not checked anywhere below.  The assembler\n");
    fprintf(fP, "cannot know which words another agent reads or writes behind the program's\n");
    fprintf(fP, "back, so a load that looks redundant may be polling and a word that looks\n");
    fprintf(fP, "dead may be a mailbox.  Timing is the same kind of thing: a delay loop for\n");
    fprintf(fP, "the typewriter, the punch or the display is meant to take the time it takes.\n");
    fprintf(fP, "Every suggestion here is subject to both, and only the author can settle them.\n");
}

// The findings, grouped by rule.  A rule with nothing to say is left out
// entirely; a report with no findings at all still says so, and the sections
// after it still carry the statistics.
static void
writeFindingList(FILE *fP, OptTableP tableP)
{
int rule;
int any;
OptFindingP findingP;

    fprintf(fP, "\nFindings: %d\n", tableP->findingCount);

    if( !tableP->findingCount )
    {
        fprintf(fP, "  No rule matched anything it could suggest a change for.\n");
        return;
    }

    fprintf(fP, "  Taken together they would remove %d word%s", tableP->savedWords,
        (tableP->savedWords == 1)?"":"s");

    if( tableP->savedTemps )
    {
        fprintf(fP, " and %d storage word%s", tableP->savedTemps, (tableP->savedTemps == 1)?"":"s");
    }

    fprintf(fP, ", and %d microsecond%s from one pass through every one of them.\n",
        tableP->savedTime, (tableP->savedTime == 1)?"":"s");
    fprintf(fP, "  Per rule:");

    for( rule = 0; rule < OPTRULE_COUNT; ++rule )
    {
        if( tableP->findingCounts[rule] )
        {
            fprintf(fP, " %s %d", optRuleName((OptRuleId)rule), tableP->findingCounts[rule]);
        }
    }

    fprintf(fP, "\n");

    for( rule = 0; rule < OPTRULE_COUNT; ++rule )
    {
        if( !tableP->findingCounts[rule] )
        {
            continue;
        }

        fprintf(fP, "\n  %-4s %s (%d)\n", optRuleName((OptRuleId)rule), optRuleSummary((OptRuleId)rule),
            tableP->findingCounts[rule]);
        any = 0;

        for( findingP = tableP->findingsP; findingP; findingP = findingP->nextP )
        {
            if( findingP->rule != (OptRuleId)rule )
            {
                continue;
            }

            printFinding(fP, findingP, 0);
            any = 1;
        }

        if( !any )
        {
            fprintf(fP, "    (none)\n");
        }
    }
}

// The patterns that matched and were then refused, grouped by rule, with a
// tally by reason first.  Study section 7 calls this the more informative
// half: it is where the self-modifying code, the dispatch tables and the
// shared scratch words show up.
static void
writeSuppressedList(FILE *fP, OptTableP tableP)
{
int rule;
int why;
OptFindingP findingP;

    fprintf(fP, "\nSuppressed findings: %d\n", tableP->suppressedCount);

    if( !tableP->suppressedCount )
    {
        return;
    }

    fprintf(fP, "  These patterns matched and were refused.  Each line says which\n");
    fprintf(fP, "  precondition, or which fact about the words themselves, blocked it.\n");
    fprintf(fP, "  Per reason:");

    for( why = 0; why < OPTWHY_COUNT; ++why )
    {
        if( tableP->whyCounts[why] )
        {
            fprintf(fP, " %s %d", optWhyName((OptWhy)why), tableP->whyCounts[why]);
        }
    }

    fprintf(fP, "\n");

    for( rule = 0; rule < OPTRULE_COUNT; ++rule )
    {
        if( !tableP->suppressedCounts[rule] )
        {
            continue;
        }

        fprintf(fP, "\n  %-4s %s (%d)\n", optRuleName((OptRuleId)rule), optRuleSummary((OptRuleId)rule),
            tableP->suppressedCounts[rule]);

        for( findingP = tableP->suppressedP; findingP; findingP = findingP->nextP )
        {
            if( findingP->rule != (OptRuleId)rule )
            {
                continue;
            }

            printFinding(fP, findingP, 1);
        }
    }
}

// The statistics: what the program is made of, per bank.  The word counts and
// the decode and spelling breakdowns are task A1's and A2's, the references
// and the classification task A3's, the blocks and reachability task A4's,
// and the instruction mix and the static time are this task's.
static void
writeStatistics(FILE *fP, OptTableP tableP)
{
int i;
int bank;
int kindCounts[OPTK_CONST + 1];
int groupCounts[OPTG_1D + 1];
int spellingCounts[OPTS_OTHER + 1];
int labelCount;
int spelled1D;
int unspelled1D;
int instructions;
int memrefs;
int indirects;
int staticTime;
int waits;
int xcts;
OptWordP entryP;
OptBankP bankP;
OptLabelP labelP;
OptLabelDefP defP;
int unattached;

    fprintf(fP, "\nStatistics\n");
    fprintf(fP, "  Word table: %d words", tableP->count);

    if( tableP->reservedCount )
    {
        fprintf(fP, ", %d reserved by table with no initializer", tableP->reservedCount);
    }

    if( tableP->dupCount )
    {
        fprintf(fP, ", %d at overlaid addresses", tableP->dupCount);
    }

    fprintf(fP, "\n");

    if( tableP->mismatchCount )
    {
        fprintf(fP, "WARNING: %d words whose parser-assigned address differs from the binary generator's\n",
            tableP->mismatchCount);
    }

    for( bank = 0; bank <= MAXBANK; ++bank )
    {
        if( !(bankP = tableP->banksP[bank]) )
        {
            continue;
        }

        for( i = 0; i <= OPTK_CONST; ++i )
        {
            kindCounts[i] = 0;
        }

        for( i = 0; i <= OPTG_1D; ++i )
        {
            groupCounts[i] = 0;
        }

        for( i = 0; i <= OPTS_OTHER; ++i )
        {
            spellingCounts[i] = 0;
        }

        labelCount = 0;
        spelled1D = 0;
        unspelled1D = 0;
        instructions = 0;
        memrefs = 0;
        indirects = 0;
        staticTime = 0;
        waits = 0;
        xcts = 0;

        for( i = 0; i < tableP->count; ++i )
        {
            entryP = tableP->entriesPP[i];

            if( entryP->bank != bank )
            {
                continue;
            }

            ++kindCounts[entryP->kind];

            for( labelP = entryP->labelsP; labelP; labelP = labelP->nextP )
            {
                ++labelCount;
            }

            // The decoded view (task A2): reserved words have none.
            if( entryP->flags & OPTF_RESERVED )
            {
                continue;
            }

            ++groupCounts[entryP->decode.group];
            ++spellingCounts[entryP->decode.spelling];

            if( entryP->decode.spelled1D )
            {
                ++spelled1D;
            }
            else if( entryP->decode.uses1D )
            {
                ++unspelled1D;
            }

            // The instruction mix and the static time count the words the
            // program is believed to execute -- the ones the control-flow
            // graph holds -- and not the words whose bits merely decode.
            if( !inGraph(entryP) )
            {
                continue;
            }

            ++instructions;
            staticTime += optWordTime(entryP);

            if( entryP->decode.group == OPTG_MEMREF )
            {
                ++memrefs;

                if( entryP->decode.memIndirect )
                {
                    ++indirects;
                }

                if( entryP->decode.opcode == 010 )
                {
                    ++xcts;
                }
            }
            else if( (entryP->decode.group == OPTG_IOT) && entryP->decode.iotWait )
            {
                ++waits;
            }
        }

        fprintf(fP, "  bank %2d: %5d words, %d labels;", bank, bankP->count, labelCount);

        for( i = 0; i <= OPTK_CONST; ++i )
        {
            if( kindCounts[i] )
            {
                fprintf(fP, " %s %d", kindName((OptKind)i), kindCounts[i]);
            }
        }

        fprintf(fP, "\n");

        // What the bits decode to, whatever the words are for.
        fprintf(fP, "    decoded:");

        for( i = 0; i <= OPTG_1D; ++i )
        {
            if( groupCounts[i] )
            {
                fprintf(fP, " %s %d", optGroupName((OptGroup)i), groupCounts[i]);
            }
        }

        fprintf(fP, "\n    spelled:");

        for( i = 0; i <= OPTS_OTHER; ++i )
        {
            if( spellingCounts[i] )
            {
                fprintf(fP, " %s %d", optSpellingName((OptSpelling)i), spellingCounts[i]);
            }
        }

        fprintf(fP, "\n");

        if( spelled1D || unspelled1D )
        {
            fprintf(fP, "    PDP-1D: %d words spelled with a PDP-1D mnemonic, %d words carry PDP-1D-only bits without one\n",
                spelled1D, unspelled1D);
        }

        // The references and the classification (task A3).
        writeReferenceReport(fP, tableP, bank);

        // The control-flow overlay (task A4).
        writeFlowReport(fP, tableP, bank);

        // The instruction mix and the static time (task A5).
        fprintf(fP, "    instructions: %d, %d memory reference (%d indirect), %d non-memory; static time %d us\n",
            instructions, memrefs, indirects, (instructions - memrefs), staticTime);

        if( xcts || waits )
        {
            fprintf(fP, "      the static time understates this bank: %d xct%s do not include the word they run, and %d iot%s wait for a device\n",
                xcts, (xcts == 1)?"":"s", waits, (waits == 1)?"":"s");
        }
    }

    fprintf(fP, "  Times are the F-15D handbook's: 5 us a memory cycle, 10 us a two-cycle\n");
    fprintf(fP, "  memory reference, 5 us the jump and augmented instructions, 5 us more for\n");
    fprintf(fP, "  one indirect step, mul 25 and div 40 (the handbook's maxima).  A word\n");
    fprintf(fP, "  counted once here may of course run any number of times.\n");

    // Labels that name an address nothing was emitted at.
    unattached = 0;

    for( defP = tableP->labelsP; defP; defP = defP->nextP )
    {
        if( !defP->attached )
        {
            ++unattached;
        }
    }

    if( unattached )
    {
        fprintf(fP, "  %d labels name an address with no emitted word\n", unattached);
    }

    if( tableP->unresolvedCount )
    {
        fprintf(fP, "  %d references to symbols that never resolved were skipped (see stderr)\n",
            tableP->unresolvedCount);
    }

    if( tableP->noWordCount )
    {
        fprintf(fP, "  %d references name an address at which nothing was emitted\n", tableP->noWordCount);
    }

    if( tableP->hasStart )
    {
        fprintf(fP, "  Program starts at 0%04o\n", tableP->startAddr);
    }
    else
    {
        fprintf(fP, "  Program ends with stop, no start address\n");
    }
}

// Print the rule dump (-O=rules): one line per finding, the live ones first
// and then the suppressed ones, each carrying everything the report says
// about it in a form a test can compare.  The format is
//
//   live RULE bBANK ADDR nWORDS wWORDS tTEMPS uTIME | word ; word => replace | note
//   supp RULE bBANK ADDR REASON | word ; word => replace | evidence
//
// where a word is its address, its value and its source spelling.  The last
// line is a summary, so a test can check the counts without parsing the rest.
void
optDumpRules(FILE *fP, OptTableP tableP)
{
int i;
int pass;
OptFindingP findingP;
OptWordP entryP;
char spell[OPTMSG_SIZE];

    for( pass = 0; pass < 2; ++pass )
    {
        findingP = (pass)?tableP->suppressedP:tableP->findingsP;

        for( ; findingP; findingP = findingP->nextP )
        {
            fprintf(fP, "%s %-4s b%d %04o", (pass)?"supp":"live", optRuleName(findingP->rule),
                findingP->bank, findingP->addr);

            if( pass )
            {
                fprintf(fP, " %s", optWhyName(findingP->why));
            }
            else
            {
                fprintf(fP, " n%d w%d t%d u%d%s", findingP->wordCount, findingP->saveWords,
                    findingP->saveTemps, findingP->saveTime, (findingP->perHop)?"/hop":"");
            }

            fprintf(fP, " |");

            for( i = 0; i < findingP->wordCount; ++i )
            {
                entryP = findingP->wordsP[i];
                spellWord(entryP, spell, sizeof(spell));
                fprintf(fP, "%s %04o %06o %s", (i)?" ;":"", entryP->addr, entryP->value, spell);
            }

            fprintf(fP, " => %s | %s\n", (findingP->replaceP)?findingP->replaceP:"-",
                (findingP->detailP)?findingP->detailP:"-");
        }
    }

    fprintf(fP, "totals live %d supp %d words %d temps %d time %d\n",
        tableP->findingCount, tableP->suppressedCount, tableP->savedWords,
        tableP->savedTemps, tableP->savedTime);
}
