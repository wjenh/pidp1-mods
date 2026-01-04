/*
 * Process a parse tree to generate a loadable binary tape image.
*/
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "am1.h"
#include "y.tab.h"
#include "font5x7.h"
#include "xldr.h"

#define BUFSIZE     4096
#define ENDLOADER   0400000
#define WRDMSK      0777777

#define DIO         0320000
#define JMP         0600000

// A buffer for building output words
typedef struct
{
    int startAddr;
    int count;
    uint32_t buffer[BUFSIZE];
} Buffer, *BufferP;

#define bufferCount(bufP)    (bufP->count)

static Buffer outBuf;
static BufferP outBufP = &outBuf;

static int cur_pc;
static int cur_bank;

extern bool sawBank;
extern BankContextP banksP;
extern SymListP constsListP;

extern int evalExpr(PNodeP);
extern int onesComplAdj(int);
extern int twosComplAdj(int);

static void initBuffer(BufferP bufP, int startAddr);
static void putBuffer(FILE *outfP, BufferP bufP, uint32_t word);
static void flushBuffer(FILE *outfP, BufferP bufP);

static void writeWord(FILE *, uint32_t);
static void writeLabel(FILE *, char *);
static void writeRIM(FILE *, uint32_t addr, uint32_t instr);
static void writeLoader(FILE *, bool);
static void writeLoaderBlock(FILE *fP, BufferP bufP);
static void writeBlankTape(FILE *fP, int count);

static int canReduce(PNodeP);
static int reduceOperand(PNodeP);

static void writeStatements(FILE *, PNodeP);
static void writeAscii(FILE *outfP, char *strP);
static void writeText(FILE *outfP, char *strP);
static void writeVars(FILE *outfP, PNodeP nodeP);
static void writeConstants(FILE *outfP, SymNodeP nodeP);

void verror(char *msgP, ...);

// Walk a tree and emit a binary tape image
int
binCodegen(FILE *outfP, PNodeP rootP)
{
    // The root is a HEADER.
    // The root lhs is the program body, the rhs the START at the end of the program.

    // First, put out the leader
    // DEC standard was apparently 2 feet(!) of blank tape.
    // Let's not get carried away.
    writeBlankTape(outfP, 5);
    writeLabel(outfP, rootP->value.strP);
    writeBlankTape(outfP, 5);
    // Next, put out our loader.
    writeLoader(outfP, sawBank);
    writeBlankTape(outfP, 2);
    writeLabel(outfP, "AM1");
    writeBlankTape(outfP, 2);
    // Emit the code
    writeStatements(outfP, rootP->leftP);
    // Finsh up
    flushBuffer(outfP, outBufP);
    writeWord(outfP, ENDLOADER | rootP->rightP->value.ival);
    writeBlankTape(outfP, 2);
    writeLabel(outfP, "DONE");

    return(1);
}

static void
writeStatements(FILE *outfP, PNodeP nodeP)
{
int i, j;
PNodeP node2P;
BankContextP bankP;

    cur_pc = 4;                 // the macro1 default
    initBuffer(outBufP, cur_pc);

    while( nodeP )
    {
        switch( nodeP->type )
        {
        case COMMENT:           // none of these emit code or change any state
        case TERMINATOR:
            break;

        case ORIGIN:
            cur_pc =  nodeP->value.ival;
            flushBuffer(outfP, outBufP);
            initBuffer(outBufP, cur_pc);
            if( canReduce(nodeP->rightP) )
            {
                i = reduceOperand(nodeP->rightP) & WRDMSK;
                nodeP->value2.ival = i;     // save for listing
                putBuffer(outfP, outBufP, i);
                cur_pc++;
            }
            break;

        case EXPR:
            if( canReduce(nodeP->rightP) )
            {
                i = reduceOperand(nodeP->rightP) & WRDMSK;
                nodeP->value2.ival = i;     // save for listing
                putBuffer(outfP, outBufP, i);
                cur_pc++;
            }
            break;

        case LOCATION:
        case LCLLOCATION:
            if( canReduce(nodeP->rightP) )
            {
                i = reduceOperand(nodeP->rightP) & WRDMSK;
                nodeP->value2.ival = i;     // save for listing
                putBuffer(outfP, outBufP, i);
                cur_pc++;
            }
            break;

        case VARS:
            writeVars(outfP, nodeP->rightP);
            break;

        case CONSTANTS:
            writeConstants(outfP, nodeP->value.symP);
            break;

        case TEXT:
            writeText(outfP, nodeP->value.strP);
            break;

        case ASCII:
            writeAscii(outfP, nodeP->value.strP);
            break;

        case BANK:
            cur_bank = nodeP->value.ival;
            cur_pc = nodeP->value2.ival;
            flushBuffer(outfP, outBufP);
            initBuffer(outBufP, (cur_bank << 12) | cur_pc);
            break;

        case TABLE:
            if( nodeP->rightP )     // has initializer
            {
                j = evalExpr(nodeP->rightP);

                for( i = 0; i < nodeP->value.ival; ++i )
                {
                    putBuffer(outfP, outBufP, j);
                    cur_pc++;
                }
            }
            else
            {
                flushBuffer(outfP, outBufP);
                cur_pc += nodeP->value.ival;
                initBuffer(outBufP, (cur_bank << 12) | cur_pc);
            }
            break;

        default:
            // just ignore
            break;
        }

        nodeP = nodeP->leftP;
    }

    // We now have to emit any constants that didn't have an ending constants statement

    if( sawBank )       // finish trailing consts
    {
        for(BankContextP bankP = banksP; bankP; bankP = bankP->nextP)
        {
            if( bankP->constSymP )
            {
                flushBuffer(outfP, outBufP);
                cur_bank = bankP->bank;
                initBuffer(outBufP, bankP->cur_pc);
                writeConstants(outfP, bankP->constSymP);
            }
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
    case LOCAL:
    case ENDLOC:
    case FORCELOC:
    case TERMINATOR:
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

    switch( nodeP->type )
    {
    case BINOP:
        // The returned values will be in 1's cmpl
        lval = reduceOperand(nodeP->leftP);
        rval = reduceOperand(nodeP->rightP);

        switch( nodeP->value.ival )
        {
        case XOR:
            lval = lval ^ rval;
            break;

        case SEPARATOR:
        case OR:
            lval = lval | rval;
            break;

        case AND:
            lval = lval & rval;
            break;

        case DIV:
            lval = onesComplAdj(twosComplAdj(lval) / twosComplAdj(rval));
            break;

        case MOD:
            lval = onesComplAdj(twosComplAdj(lval) % twosComplAdj(rval));
            break;

        case PLUS:
            lval = onesComplAdj(twosComplAdj(lval) + twosComplAdj(rval));
            break;

        case MINUS:
            lval = onesComplAdj(twosComplAdj(lval) - twosComplAdj(rval));
            break;

        case MUL:
            lval = onesComplAdj(twosComplAdj(lval) * twosComplAdj(rval));
            break;

        default:
            verror("unknown binary op %d in reduceOperand", nodeP->value.ival);
        }

        return( lval );

    case UNOP:
        switch( nodeP->value.ival )
        {
            case PARENS:
                return( reduceOperand(nodeP->rightP) );
                break;

                case UMINUS:
            case CMPL:
                return( ~reduceOperand(nodeP->rightP) );
                break;

        default:
            verror("unknown unary op %d in reduceOperand", nodeP->value.ival);
        }
        break;

    case CONSTANT:      // don't adjust
    case OPORABLE:
    case OPCODE:
    case OPADDR:
        return( nodeP->value.symP->value );
        break;

    case DOT:
        return( cur_pc );   // also positive, don't adjust
        break;

    case LCLADDR:
        // local addrs will already be resolved to the actual location
        symP = nodeP->value.symP;
        if( symP->flags & SYMF_RESOLVED )
        {
            return( symP->value );
        }
        else
        {
            verror("local symbol %s has no defined value", symP->name);
        }
        break;

    case ADDR:
        symP = nodeP->value.symP;
        if( symP->flags & SYMF_RESOLVED )
        {
            return( symP->value );
        }
        else
        {
            verror("symbol %s has no defined value", symP->name);
        }
        break;

    case BREF:
        return( (nodeP->value2.ival << 12) | nodeP->value.symP->value );
        break;

    case CHAR:
    case FLEXO:
    case LITCHAR:
        return( nodeP->value.ival & WRDMASK );
        break;

    case INTEGER:
        return( nodeP->value.ival & WRDMASK );
        break;

    case VALUESPEC:
        return( nodeP->value.symP->value );
        break;

    default:
        verror("unknown op %d, pc 0%04o in reduceOperand", nodeP->type, nodeP->pc);
    }
}

// Emit packed ascii
static void
writeAscii(
    FILE *outfP,
    char *strP
    )
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
            putBuffer(outfP, outBufP,  word);
            ++cur_pc;
        }

        i ^= 1;
    }
    while( *strP++ );

    if( i )         // if not zero, we didn't finish writing a full word, do so with low byte 0
    {
        putBuffer(outfP, outBufP, word << 9);
        ++cur_pc;
    }
}

// Emit packed flexo code
static void
writeText(FILE *outfP, char *strP)
{
int i;
int val;

    for( val = i = 0; *strP != 0; i++ )
    {
        if( i && !(i % 3) )
        {
            putBuffer(outfP, outBufP, val);
            ++cur_pc;
        }

        val <<= 6;
        val |= *strP++;
    }

    if( i % 3 )     // had leftovers, finish the word
    {
        while( i++ % 3 )
        {
            val <<= 6;
        }

        putBuffer(outfP, outBufP, val);
        ++cur_pc;
    }
    else if( !*strP && !( i % 3) )
    {
        putBuffer(outfP, outBufP, val);
        ++cur_pc;
    }
}

// Walk a list of variables, emit the storage
static void
writeVars(FILE *fP, PNodeP nodeP)
{
int i;
SymNodeP symP;

    while( nodeP )
    {
        i = (nodeP->leftP)?reduceOperand(nodeP->leftP) & WRDMSK:0;
        putBuffer(fP, outBufP, i);
        ++cur_pc;

        nodeP = nodeP->rightP;
    }
}

// Walk a symbol table of constants, emit the values
static void
writeConstants(FILE *fP, SymNodeP symP)
{
    if( !symP )
    {
        return;
    }

    if( !(symP->flags & SYMF_EMITTED) )
    {
        symP->flags |= SYMF_EMITTED;
        putBuffer(fP, outBufP, onesComplAdj(symP->value2));
        ++cur_pc;
    }

    writeConstants(fP, symP->leftP);
    writeConstants(fP, symP->rightP);
}

// Reset a buffer to empty with a new starting address
static void
initBuffer(
    BufferP bufP,
    int startAddr
    )
{
    bufP->startAddr = (cur_bank << 12) | startAddr;
    bufP->count = 0;
}

// Add a word to the buffer, if full, flush it
static void
putBuffer(
    FILE *fP,
    BufferP bufP,
    uint32_t word
    )
{
    if( bufferCount(bufP) >= BUFSIZE )
    {
        flushBuffer(fP, bufP);
    }

    bufP->buffer[bufP->count++] = word;
}

// Flush a buffer to 'tape, reset the start addr to the current addr + the buffer count
static void
flushBuffer(
    FILE *fP,
    BufferP bufP
    )
{
int i, j;

    if( (i = bufferCount(bufP)) )
    {
        writeLoaderBlock(fP, bufP);
        bufP->startAddr += bufP->count;
        bufP->count = 0;
    }
}

// write the given number of binary 0s
void
writeBlankTape(
    FILE *fP,
    int count
    )
{
    while( count-- )
    {
        fputc(0, fP);
    }
}

// write a complete loader data block
static void
writeLoaderBlock(
    FILE *fP,
    BufferP bufP
    )
{
int i;

    // The first word is the starting address
    writeWord(fP, bufP->startAddr);
    // Then the ending address + 1
    writeWord(fP, bufP->startAddr + bufP->count);

    // followed by the data words
    for( i = 0; i < bufP->count; )
    {
        writeWord(fP, bufP->buffer[i++]);
    }

    // put in a little break
    writeBlankTape(fP, 2);
}

static void
writeLabel(
    FILE *fP,
    char *labelP
    )
{
int i, idx;
unsigned char ch;

    // Each character uses 5 bytes, each byte one column.
    // Only characters 0x20 (space) and above are expected.
    while( (ch = *labelP++) && (ch != '\n') )
    {
        ch -= 0x20;
        idx = ch * 5;
        for( i = 0; i++ < 5; )
        {
            fputc(Font5x7[idx++], fP);
        }
        // and a bit of space
        fputc(0, fP);
    }
}

// Write out the loader in RIM format.
// If extended memory is not being used, overwrite the first word which is eem with nop.
static void
writeLoader(
    FILE *fP,
    bool sawBank
    )
{
int addr;
int i;

    addr = LDR_START_ADDR;

    if( !sawBank )
    {
        writeRIM(fP, addr++, 0760000);     // nop
        i = 1;
    }
    else
    {
        i = 0;
    }

    while( i < sizeof(xloader) / sizeof(uint32_t) )
    {
        writeRIM(fP, addr++, xloader[i++]);     // nop
    }

    // And the terminating JMP
    writeWord(fP, JMP | 07751);
}

static void
writeWord(
    FILE *fP,
    uint32_t word
    )
{
    fputc(((word >> 12) & 077) | 0200, fP);
    fputc(((word >> 6) & 077) | 0200, fP);
    fputc((word & 077) | 0200, fP);
}

static void
writeRIM(
    FILE *fP,
    uint32_t addr,
    uint32_t instr
    )
{
    writeWord(fP, DIO | addr);
    writeWord(fP, instr);
}
