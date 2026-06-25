/*
 * Process a parse tree to generate a test dump file.
 * The output is each memory location that was generated printed as a 6 digit octal number.
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
static void dumpVars(FILE *outfP, PNodeListP listP, int lineNo);
static void dumpConstants(FILE *outfP, SymNodeP nodeP, int lineNo);
static bool dumpText(FILE *outfP, FlexText flexText);
static bool dumpAscii(FILE *outfP, char *strP);

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
    fprintf(outfP, "%06o\n", rootP->rightP->value.ival);
    return(1);
}

static void
dumpStatements(FILE *outfP, PNodeP nodeP)
{
int i, j;
PNodeP node2P;
BankContextP bankP;

    while( nodeP )
    {
        switch( nodeP->type )
        {
        case COMMENT:           // none of these output anything
        case TERMINATOR:
        case ORIGIN:
            break;

        case EXPR:
            if( canReduce(nodeP->rightP) )
            {
                i = reduceOperand(nodeP->rightP);
                fprintf(outfP, "%06o\n", i);
            }
            break;

        case LOCATION:
        case LCLLOCATION:
            if( canReduce(nodeP->rightP) )
            {
                // Normal case: a single-word instruction or expression follows
                // the label on the same line (e.g. "foo, jmp bar").
                i = reduceOperand(nodeP->rightP);
                fprintf(outfP, "%06o\n", i);
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
                    dumpText(outfP, nodeP->rightP->value.flexText);
                    break;

                case ASCII:
                    dumpAscii(outfP, nodeP->rightP->value.strP);
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
            dumpVars(outfP, (PNodeListP)(nodeP->value.ptr), nodeP->lineNo);
            break;

        case CONSTANTS:
            dumpConstants(outfP, nodeP->value.symP, nodeP->lineNo);
            break;

        case TEXT:
        case TYPE340:
            dumpText(outfP, nodeP->value.flexText);
            break;
            break;

        case ASCII:
            dumpAscii(outfP, nodeP->value.strP);
            break;

        case BANK:
            // Nothing to do
            break;

        case TABLE:
            if( nodeP->rightP )     // has initializer
            {
                j = evalExpr(nodeP->rightP);

                for( i = 0; i < nodeP->value.ival; ++i )
                {
                    fprintf(outfP, "%06o\n", i);
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
        if( bankP->constSymP )
        {
            dumpConstants(outfP, bankP->constSymP, -1);
        }

        if( bankP->varNodesP )
        {
            dumpVars(outfP, bankP->varNodesP, -1);
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
int lval;
int rval;
char ch;
SymNodeP symP;
PNodeP node2P;

    if( !nodeP )
    {
        return(0);
    }

    if( nodeP->type == DOT )
    {
        return( 0 );
    }

    return( evalExpr(nodeP) );
}

// Emit packed ascii, return false if memory overwritten, else true.
static bool
dumpAscii(FILE *outfP, char *strP)
{
int i;
int word;

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
            fprintf(outfP, "%06o\n", i);
        }

        i ^= 1;
    }
    while( *strP++ );

    if( i )         // if not zero, we didn't finish writing a full word, do so with low byte 0
    {
        fprintf(outfP, "%06o\n", i);
    }

    return(true);
}

// Emit packed flexo code
static bool
dumpText(FILE *outfP, FlexText flexText)
{
int i;
int val;
char *bufP;

    bufP = flexText.bufP;

    for( val = i = 0; i < flexText.nchars; i++ )
    {
        if( i && !(i % 3) )
        {
            fprintf(outfP, "%06o\n", val);
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

        fprintf(outfP, "%06o\n", val);
    }
    else if( (i >= flexText.nchars) && !( i % 3) )
    {
        fprintf(outfP, "%06o\n", val);
    }

    return(true);
}

// Walk a list of variables, emit the storage.
// If lineNo is -1, this is being called to automatically emit vars that were't emitted explicitly.
static void
dumpVars(FILE *fP, PNodeListP listP, int lineNo)
{
int i;
PNodeP nodeP;
SymNodeP symP;

    while( listP )
    {
        nodeP = listP->nodeP;

        i = (nodeP->leftP)?reduceOperand(nodeP->leftP):0;
        fprintf(fP, "%06o\n", i);
        listP = listP->nextP;
    }
}

// Walk a symbol table of constants, emit the values.
// If lineNo is -1, this is being called to automatically emit vars that were't emitted explicitly.
static void
dumpConstants(FILE *fP, SymNodeP symP, int lineNo)
{
    if( !symP )
    {
        return;
    }

    if( !(symP->flags & SYMF_EMITTED) )
    {
        symP->flags |= SYMF_EMITTED;
        fprintf(fP, "%06o\n", symP->value2);
    }

    dumpConstants(fP, symP->leftP, lineNo);
    dumpConstants(fP, symP->rightP, lineNo);
}
