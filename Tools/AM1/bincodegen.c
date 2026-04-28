/*
 * Process a parse tree to generate a loadable binary tape image.
 * Check for memory overwrites also.
*/
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "am1.h"
#include "y.tab.h"
#include "font5x7.h"
#include "xldr.h"
#include "type340chars.h"

#define BUFSIZE     4096
#define ENDLOADER   0400000
#define STPLOADER   0600000

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

extern int lineno;      // used by verror() for a line number
extern bool sawBank;
extern bool noRim;
extern bool noMemFatal;
extern bool keepMinusZero;
extern BankContextP banksP;

extern int evalExpr(PNodeP);
extern int onesComplAdj(int);
extern int twosComplAdj(int);
extern void leave(int);
extern void vwarn(int errtype, const char *msgP, ...);

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
static void adjustPC(int);

static void writeStatements(FILE *, PNodeP);
static void writeVars(FILE *outfP, PNodeListP listP, int lineNo);
static void writeConstants(FILE *outfP, SymNodeP nodeP, int lineNo);
static bool writeText(FILE *outfP, FlexText flexText);
static bool writeType340(FILE *outfP, char *strP);
static bool writeAscii(FILE *outfP, char *strP);
static bool setBit(uint64_t map[], int addr);
static bool setBits(uint64_t map[], int addr, int count);

void verror(char *msgP, ...);

// This is a bitmap for tracking used memory locations.
// Each bit represents one word in memory.
uint64_t memMap[((MAXBANK + 1) * BANKSIZE) / sizeof(uint64_t)];

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
    if( !noRim )
    {
        writeLoader(outfP, sawBank);
        writeBlankTape(outfP, 2);
    }
    writeLabel(outfP, AM1SHORTVERSION);
    writeBlankTape(outfP, 2);
    writeLabel(outfP, "wje");       // for posterity. :)
    writeBlankTape(outfP, 2);
    // Emit the code
    writeStatements(outfP, rootP->leftP);
    // Finsh up
    flushBuffer(outfP, outBufP);
    if( rootP->rightP->type == START )
    {
        writeWord(outfP, ENDLOADER | rootP->rightP->value.ival);
    }
    else
    {
        writeWord(outfP, STPLOADER);     // stop instead of start
    }
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
            cur_pc =  nodeP->value.ival & ADDRMASK;
            flushBuffer(outfP, outBufP);
            initBuffer(outBufP, cur_pc);
            if( canReduce(nodeP->rightP) )
            {
                i = reduceOperand(nodeP->rightP);
                nodeP->value2.ival = i;     // save for listing
                putBuffer(outfP, outBufP, i);
                adjustPC(1);
            }
            break;

        case EXPR:
            if( canReduce(nodeP->rightP) )
            {
                i = reduceOperand(nodeP->rightP);
                nodeP->value2.ival = i;     // save for listing
                if( !setBit(memMap, (cur_bank << 12) | cur_pc) )
                {
                    lineno = nodeP->lineNo;
                    if( noMemFatal )
                    {
                        vwarn(WARN_MEMORY, "Already used memory address 0%04o would be overwritten.", cur_pc);
                    }
                    else
                    {
                        verror("Already used memory address 0%04o would be overwritten.", cur_pc);
                    }
                }
                putBuffer(outfP, outBufP, i);
                adjustPC(1);
            }
            break;

        case LOCATION:
        case LCLLOCATION:
            if( canReduce(nodeP->rightP) )
            {
                i = reduceOperand(nodeP->rightP);
                nodeP->value2.ival = i;     // save for listing
                putBuffer(outfP, outBufP, i);
                adjustPC(1);
            }
            break;

        case VARS:
            writeVars(outfP, (PNodeListP)(nodeP->value.ptr), nodeP->lineNo);
            break;

        case CONSTANTS:
            writeConstants(outfP, nodeP->value.symP, nodeP->lineNo);
            break;

        case TEXT:
            if( !writeText(outfP, nodeP->value.flexText) )
            {
                lineno = nodeP->lineNo;
                verror("Already used memory address 0%04o would be overwritten by text.", cur_pc);
            }
            break;

        case TYPE340:
            if( !writeType340(outfP, nodeP->value.strP) )
            {
                lineno = nodeP->lineNo;
                verror("Already used memory address 0%04o would be overwritten by type340.", cur_pc);
            }
            break;

        case ASCII:
            if( !writeAscii(outfP, nodeP->value.strP) )
            {
                lineno = nodeP->lineNo;
                verror("Already used memory address 0%04o would be overwritten by ascii.", cur_pc);
            }
            break;

        case BANK:
            cur_bank = nodeP->value.ival;
            cur_pc = nodeP->value2.ival & ADDRMASK;
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
                    adjustPC(1);
                }
            }
            else
            {
                flushBuffer(outfP, outBufP);
                if( !setBits(memMap, (cur_bank << 12) | cur_pc, nodeP->value.ival) )
                {
                    lineno = nodeP->lineNo;
                    verror("Already used memory address 0%04o would be overwritten by table.", cur_pc);
                }
                adjustPC(nodeP->value.ival);
                initBuffer(outBufP, (cur_bank << 12) | cur_pc);
            }
            break;

        default:
            // just ignore
            break;
        }

        nodeP = nodeP->leftP;
    }

    // We now have to emit any constants and vars that didn't have an ending constants statement
    for(BankContextP bankP = banksP; bankP; bankP = bankP->nextP)
    {
        if( bankP->constSymP || bankP->varNodesP )
        {
            // prepare to write one or both
            flushBuffer(outfP, outBufP);
            cur_bank = bankP->bank;
            initBuffer(outBufP, bankP->cur_pc);
        }

        if( bankP->constSymP )
        {
            writeConstants(outfP, bankP->constSymP, -1);
        }

        if( bankP->varNodesP )
        {
            writeVars(outfP, bankP->varNodesP, -1);
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
    case ADDLOCAL:
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

    if( nodeP->type == DOT )
    {
        return( cur_pc );
    }

    return( evalExpr(nodeP) );
}

// Emit packed ascii, return false if memory overwritten, else true.
static bool
writeAscii(FILE *outfP, char *strP)
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
            if( !setBit(memMap, (cur_bank << 12) | cur_pc) )
            {
                return(false);
            }

            word = (word << 9) | *strP;
            putBuffer(outfP, outBufP,  word);
            adjustPC(1);
        }

        i ^= 1;
    }
    while( *strP++ );

    if( i )         // if not zero, we didn't finish writing a full word, do so with low byte 0
    {
        putBuffer(outfP, outBufP, word << 9);
        adjustPC(1);
    }

    return(true);
}

// Emit packed flexo code
static bool
writeText(FILE *outfP, FlexText flexText)
{
int i;
int val;
char *bufP;

    bufP = flexText.bufP;

    for( val = i = 0; i < flexText.nchars; i++ )
    {
        if( i && !(i % 3) )
        {
            if( !setBit(memMap, (cur_bank << 12) | cur_pc) )
            {
                return(false);
            }

            putBuffer(outfP, outBufP, val);
            adjustPC(1);
        }

        val <<= 6;
        val |= *bufP++;
    }

    if( i % 3 )     // had leftovers, finish the word
    {
        if( !setBit(memMap, (cur_bank << 12) | cur_pc) )
        {
            return(false);
        }

        while( i++ % 3 )
        {
            val <<= 6;
        }

        putBuffer(outfP, outBufP, val);
        adjustPC(1);
    }
    else if( (i >= flexText.nchars) && !( i % 3) )
    {
        if( !setBit(memMap, (cur_bank << 12) | cur_pc) )
        {
            return(false);
        }

        putBuffer(outfP, outBufP, val);
        adjustPC(1);
    }

    return(true);
}

// Emit packed type 340 code, very similar to text above.
static bool
writeType340(FILE *outfP, char *strP)
{
int i;
int val;

    for( val = i = 0;; )
    {
        val <<= 6;
        val |= *strP;

        if( i == 2 )
        {
            if( !setBit(memMap, (cur_bank << 12) | cur_pc) )
            {
                return(false);
            }

            putBuffer(outfP, outBufP, val);
            adjustPC(1);
        }

        if( ++i > 2 )
        {
            i = 0;
        }

        if( *strP++ == TYPE340END )
        {
            break;
        }
    }

    if( i != 0 )        // had leftovers, finish the word
    {
        if( !setBit(memMap, (cur_bank << 12) | cur_pc) )
        {
            return(false);
        }

        while( i++ != 3 )
        {
            val <<= 6;
        }

        putBuffer(outfP, outBufP, val);
        adjustPC(1);
    }

    return(true);
}

// Walk a list of variables, emit the storage.
// If lineNo is -1, this is being called to automatically emit vars that were't emitted explicitly.
static void
writeVars(FILE *fP, PNodeListP listP, int lineNo)
{
int i;
PNodeP nodeP;
SymNodeP symP;

    while( listP )
    {
        nodeP = listP->nodeP;

        if( !setBit(memMap, (cur_bank << 12) | cur_pc) )
        {
            if( lineNo == -1 )
            {
                verror(
            "Already used memory would be overwritten by automatically emitted variables at memory address 0%4o.\n",
                    cur_pc);
            }
            else
            {
                lineno = lineNo;
                verror("Already used memory would be overwritten by variables at memory address 0%4o.\n",
                    cur_pc);
            }
        }

        i = (nodeP->leftP)?reduceOperand(nodeP->leftP):0;
        putBuffer(fP, outBufP, i);
        adjustPC(1);

        listP = listP->nextP;
    }
}

// Walk a symbol table of constants, emit the values.
// If lineNo is -1, this is being called to automatically emit vars that were't emitted explicitly.
static void
writeConstants(FILE *fP, SymNodeP symP, int lineNo)
{
    if( !symP )
    {
        return;
    }

    if( !(symP->flags & SYMF_EMITTED) )
    {
        if( !setBit(memMap, (cur_bank << 12) | cur_pc) )
        {
            if( lineNo == -1 )
            {
                verror(
            "Already used memory would be overwritten by automatically emitted constants at memory address 0%4o.\n",
                    cur_pc);
            }
            else
            {
                lineno = lineNo;
                verror("Already used memory would be overwritten by constants at memory address 0%4o.\n",
                    cur_pc);
            }
        }

        symP->flags |= SYMF_EMITTED;
        putBuffer(fP, outBufP, symP->value2);
        adjustPC(1);
    }

    writeConstants(fP, symP->leftP, lineNo);
    writeConstants(fP, symP->rightP, lineNo);
}

// Add a value to the current pc, mask to 12 bits
void
adjustPC(int incr)
{
    cur_pc += incr;
    if( cur_pc > ADDRMASK )
    {
        cur_pc = 0;
    }
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

// Set the bit in the memory map corresponding to the address passed.
// If it was not already set, return true, else if already set, false.
bool
setBit(uint64_t map[], int addr)
{
int idx;
uint64_t bit;

    idx = addr >> 6;            // each map entry is 64 memory locations
    bit = UINT64_C(1) << (addr & 63);

    if( map[idx] & bit )
    {
        return( false );
    }

    map[idx] |= bit;
    return(true);
}

// Set the bits in the memory map corresponding to the address passed and the number of locations to mark.
// If it was not already set, return true, else if already set, false.
bool
setBits(uint64_t map[], int addr, int count)
{
    while( count > 0 )
    {
        if( !setBit(map, addr + (--count)) )
        {
            return(false);
        }
    }

    return(true);
}
