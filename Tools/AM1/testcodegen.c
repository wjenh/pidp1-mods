/*
 * Process a parse tree to generate a test dump file.
 * The output is each memory location and value that was generated printed as two space-separated
 * 6 digit octal numbers.
 *
*/
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "am1.h"
#include "y.tab.h"
#include "font5x7.h"
#include "type340chars.h"

extern BankContextP banksP;

extern int evalExpr(PNodeP);
extern int onesComplAdj(int);
extern int twosComplAdj(int);
extern void leave(int);

static int canReduce(PNodeP);
static int reduceOperand(PNodeP);

static void dumpStatements(FILE *, PNodeP);
static void dumpVars(FILE *outfP, PNodeP);
static void dumpConstants(FILE *fP, PNodeP nodeP, SymNodeP symP);
static void dumpText(FILE *outfP, PNodeP nodeP);
static void dumpAscii(FILE *outfP, PNodeP nodeP);
static void printOne(FILE *fP, PNodeP nodeP, int value);

// Walk a tree and emit the words.
int
testCodegen(FILE *outfP, PNodeP rootP)
{
    // Test mode is a special case.
    // Instead of actual binary in rim format, each 18 bit word is just printed as an octal number in ascii.
    // The root is a HEADER.
    // The root lhs is the program body, the rhs the START at the end of the program.
    dumpStatements(outfP, rootP->leftP);

    // We print the value of the START/END, it's not actually a memory value but used for validation.
    fprintf(outfP, "%06o %06o\n", rootP->rightP->value.ival, rootP->rightP->value.ival);
    return(1);
}

static void
dumpStatements(FILE *outfP, PNodeP nodeP)
{
int i, j;
BankContextP bankP;

    while( nodeP )
    {
        switch( nodeP->type )
        {
        case COMMENT:           // none of these output anything
        case TERMINATOR:
            break;

        case ORIGIN:
        case EXPR:
            if( canReduce(nodeP->rightP) )
            {
                i = reduceOperand(nodeP->rightP);
                printOne(outfP, nodeP->rightP, i);
            }
            break;

        case LOCATION:
        case LCLLOCATION:
            if( canReduce(nodeP->rightP) )
            {
                // Normal case: a single-word instruction or expression follows
                // the label on the same line (e.g. "foo, jmp bar").
                i = reduceOperand(nodeP->rightP);
                printOne(outfP, nodeP->rightP, i);
            }
            else if( nodeP->rightP )
            {
                // Text-directive case: labelTrailer matched a TEXT, ASCII, or
                // TYPE340 directive on the same line as the label
                // (e.g. "msg, text \"hello\"").  The write functions advance
                // cur_pc themselves for the full string length; no adjustPC()
                // call is needed here.
                switch( nodeP->rightP->type )
                {
                case TEXT:
                case TYPE340:
                    dumpText(outfP, nodeP->rightP);
                    break;

                case ASCII:
                    dumpAscii(outfP, nodeP->rightP);
                    break;

                default:
                    // canReduce returned 0 for something other than a text type
                    // (e.g. a pure-directive expression like local/endloc).
                    // Nothing to emit.
                    break;
                }
            }
            break;

        case VARS:
            dumpVars(outfP, nodeP);
            break;

        case CONSTANTS:
            dumpConstants(outfP, nodeP, nodeP->value.symP);
            break;

        case TEXT:
        case TYPE340:
            dumpText(outfP, nodeP);
            break;

        case ASCII:
            dumpAscii(outfP, nodeP);
            break;

        case BANK:
            // Nothing to do
            break;

        case TABLE:
            if( nodeP->rightP )     // has initializer
            {
                // Every word of the table gets the initializer's value, evaluated once.
                j = evalExpr(nodeP->rightP);

                for( i = 0; i < nodeP->value.ival; ++i )
                {
                    fprintf(outfP, "%02o%04o %06o\n", nodeP->bank, (nodeP->pc + i), j);
                }
            }
            break;

        default:
            // just ignore
            break;
        }

        nodeP = nodeP->leftP;
    }

    // We now have to emit any constants and vars that didn't have an ending constants statement
    for(bankP = banksP; bankP; bankP = bankP->nextP)
    {
    PNode node;

        if( bankP->constSymP )
        {
            node.pc = bankP->constPC;
            node.bank = bankP->bank;
            dumpConstants(outfP, &node, bankP->constSymP);
        }

        if( bankP->varNodesP )
        {
            node.pc = bankP->varPC;
            node.bank = bankP->bank;
            node.value.ptr = bankP->varNodesP;
            dumpVars(outfP, &node);
        }
    }
}

// some exprs don't emit anything
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

    // TEXT, ASCII, and TYPE340 nodes carry multi-word string payloads; they
    // cannot be reduced to a single integer value by reduceOperand().  They
    // appear here when labelTrailer matched a text directive on the same line
    // as a label (e.g. "msg, text \"hello\"").  The LOCATION handler below
    // has a separate branch that dispatches them to writeText/writeAscii.
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

// The real work.
// All values have to be adjusted for 1's cmpl
static int
reduceOperand(PNodeP nodeP)
{
    if( !nodeP )
    {
        return(0);
    }

    if( nodeP->type == DOT )
    {
        // A bare '.' is the address of the word itself, which the parser
        // snapshotted into the node when it built it; bincodegen.c uses its
        // running pc here and the two are the same address.
        return( nodeP->value.ival );
    }

    return( evalExpr(nodeP) );
}

// Emit packed ascii, two 9-bit characters per word, high character first, the
// terminating NUL included, a lone final character padded with a zero low byte.
// This is writeAscii() in bincodegen.c with the output replaced by printing.
// The node is the ASCII node: its pc is the first word's address.
static void
dumpAscii(FILE *outfP, PNodeP nodeP)
{
int i;
int word;
int addr;
char *strP;

    strP = nodeP->value.strP;
    addr = nodeP->pc;
    word = 0;
    i = 0;      // 0 is doing high byte, 1 doing low byte

    do
    {
        if( !i )
        {
            word = *strP;
        }
        else
        {
            word = (word << 9) | *strP;
            fprintf(outfP, "%02o%04o %06o\n", nodeP->bank, addr++, (word & WRDMASK));
        }

        i ^= 1;
    }
    while( *strP++ );

    if( i )         // if not zero, we didn't finish writing a full word, do so with low byte 0
    {
        fprintf(outfP, "%02o%04o %06o\n", nodeP->bank, addr++, ((word << 9) & WRDMASK));
    }
}

// Emit packed flexo code, three 6-bit characters per word, high character first,
// a short final word padded on the right, an empty string still one zero word.
// This is writeText() in bincodegen.c with the output replaced by printing.
// writeText() never clears its accumulator between words, so every word after
// the first carries earlier characters above bit 17; the tape writer drops them
// and this masks them the same way.
// The node is the TEXT or TYPE340 node: its pc is the first word's address.
static void
dumpText(FILE *outfP, PNodeP nodeP)
{
int i;
int val;
int addr;
char *bufP;
FlexText flexText;

    flexText = nodeP->value.flexText;
    bufP = flexText.bufP;
    addr = nodeP->pc;

    for( val = i = 0; i < flexText.nchars; i++ )
    {
        if( i && !(i % 3) )
        {
            fprintf(outfP, "%02o%04o %06o\n", nodeP->bank, addr++, (val & WRDMASK));
        }

        val <<= 6;
        val |= *bufP++;
    }

    if( i % 3 )     // had leftovers, finish the word
    {
        while( i++ % 3 )
        {
            val <<= 6;
        }

        fprintf(outfP, "%02o%04o %06o\n", nodeP->bank, addr++, (val & WRDMASK));
    }
    else if( (i >= flexText.nchars) && !( i % 3) )
    {
        fprintf(outfP, "%02o%04o %06o\n", nodeP->bank, addr++, (val & WRDMASK));
    }
}

// Walk a list of variables, emit the storage.
// If lineNo is -1, this is being called to automatically emit vars that were't emitted explicitly.
static void
dumpVars(FILE *fP, PNodeP nodeP)
{
int i;
PNodeListP listP;
SymNodeP symP;

    listP = (PNodeListP)(nodeP->value.ptr);
    while( listP )
    {
        nodeP = listP->nodeP;
        symP = nodeP->value.symP;

        i = (nodeP->leftP)?reduceOperand(nodeP->leftP):0;
        printOne(fP, nodeP, i);
        listP = listP->nextP;
    }
}

// Walk a symbol table of constants, emit the values.
static void
dumpConstants(FILE *fP, PNodeP nodeP, SymNodeP symP)
{
    if( !symP )
    {
        return;
    }

    if( !(symP->flags & SYMF_EMITTED) )
    {
        symP->flags |= SYMF_EMITTED;
        printOne(fP, nodeP, symP->value2);
        nodeP->pc++;    // because we only get the intial node
    }

    dumpConstants(fP, nodeP, symP->leftP);
    dumpConstants(fP, nodeP, symP->rightP);
}

// Print an address/value pair using the addressing information in the passed node.
static void
printOne(FILE *fP, PNodeP nodeP, int value)
{
    fprintf(fP, "%02o%04o %06o\n", nodeP->bank, nodeP->pc, value);
}
