/* optstrings.c - the am1 optimizer's spellings and evidence sentences
 *
 * Builds the text the findings and the report are made of: a word spelled as
 * its source spelled it, an operate word as its micro-ops in hardware order, a
 * skip value as its conditions, and the allocated strings a finding carries.
 * Nothing here decides anything; it writes down what optrules.c and
 * optreport.c decided, and reads the parse tree without changing it.  Depends
 * on am1.h, y.tab.h and optimizer.h.  Single threaded; an allocation failure
 * is fatal.
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

static int appendStr(char *bufP, int size, int at, const char *strP);
static int appendOctal(char *bufP, int size, int at, int value);
static const char *binopText(int op);
static int spellExpr(char *bufP, int size, int at, PNodeP nodeP);
static int spellOperandExpr(char *bufP, int size, int at, PNodeP nodeP);
static PNodeP firstConstInner(PNodeP nodeP);
static void countConstLeaves(PNodeP nodeP, int *symsP, int *othersP);
static int constMayMove(PNodeP nodeP);

// Format a string into freshly allocated memory, truncated at OPTMSG_SIZE; out
// of memory is fatal.
// Returns the string, which the caller owns.
char *
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

// Append an 18-bit value as an octal literal with the 0o prefix: am1 reads a bare
// number in the current radix, so a pasted suggestion would mean something else
// under 'decimal'.  Returns the new offset.
static int
appendOctal(char *bufP, int size, int at, int value)
{
char tmp[32];

    sprintf(tmp, "0o%o", (value & WRDMASK));
    return( appendStr(bufP, size, at, tmp) );
}

// The am1 source text of a binary operator; a space is or, so SEPARATOR is one.
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

// Render an expression tree as am1 source: emitOperand() from maccodegen.c for
// am1's own syntax, without macro1's reductions, so [..], ~ and symbol names
// stay as written.  Returns the new offset.
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
        // holds only the first expression interned in the slot, and literals
        // pool by value, so the symbol could spell another reference's text.
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

// Render only the operand: everything but the mnemonic and its i.  A binary
// operator is dropped with whichever side contributed nothing, so "sad foo"
// spells as "foo", not " foo".  Returns the new offset.
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

// Render a word as its source spelled it.  A word with no expression tree (a
// text, ascii or type340 word, a reserved table word, an uninitialized
// variable) gets its symbol's name instead, and failing that a dash.
void
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
void
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
        // No symbol in the field (lac 100, or data that decodes as a memory
        // reference): the decoded address is what the machine will use.
        appendOctal(bufP, size, 0, entryP->decode.address);
    }
}

// Find the first [..] constant reference in an expression tree.
// Returns the reference's own inner expression, or NILP when there is none.
static PNodeP
firstConstInner(PNodeP nodeP)
{
PNodeP innerP;

    if( !nodeP )
    {
        return(NILP);
    }

    if( nodeP->type == CONSTANT )
    {
        return( (PNodeP)(nodeP->value2.ptr) );
    }

    if( nodeP->type == BINOP )
    {
        if( (innerP = firstConstInner(nodeP->leftP)) )
        {
            return(innerP);
        }

        return( firstConstInner(nodeP->rightP) );
    }

    if( nodeP->type == UNOP )
    {
        return( firstConstInner(nodeP->rightP) );
    }

    return(NILP);
}

// The expression a T8 rewrite read its value from, for relayout to re-evaluate
// after words move.  Returns the first [..]'s inner expression, or NILP.
PNodeP
optFirstConstInner(PNodeP exprP)
{
    return( firstConstInner(exprP) );
}

// Could the value of an expression's first [..] change when words move?  Any
// leaf but a literal is taken to move, which at worst refuses a safe copy.
// Returns 1 when it could, 0 for literals only or no constant.
int
optConstMayMove(PNodeP exprP)
{
    return( constMayMove(firstConstInner(exprP)) );
}

// constMayMove() for one subtree: 1 if any leaf is not a literal, else 0.
static int
constMayMove(PNodeP nodeP)
{
    if( !nodeP )
    {
        return(0);
    }

    switch( nodeP->type )
    {
    case BINOP:
        return( constMayMove(nodeP->leftP) || constMayMove(nodeP->rightP) );

    case UNOP:
        return( constMayMove(nodeP->rightP) );

    case INTEGER:
    case CHAR:
    case FLEXO:
    case LITCHAR:
        return(0);

    default:
        return(1);
    }
}

// Count an expression's leaves: address symbols in *symsP, and in *othersP any
// mnemonic, modifier or value symbol (3s), complement, nested constant or
// bank-qualified reference, which means the text is not an address for law.
static void
countConstLeaves(PNodeP nodeP, int *symsP, int *othersP)
{
    if( !nodeP )
    {
        return;
    }

    switch( nodeP->type )
    {
    case BINOP:
        countConstLeaves(nodeP->leftP, symsP, othersP);
        countConstLeaves(nodeP->rightP, symsP, othersP);
        break;

    case UNOP:
        if( (nodeP->value.ival == PARENS) || (nodeP->value.ival == UMINUS) )
        {
            countConstLeaves(nodeP->rightP, symsP, othersP);
        }
        else
        {
            ++*othersP;
        }
        break;

    case ADDR:
    case LCLADDR:
        ++*symsP;
        break;

    case INTEGER:
    case CHAR:
    case FLEXO:
    case LITCHAR:
    case DOT:
        break;

    default:
        ++*othersP;
        break;
    }
}

// Render the inside of a word's first [..] when it names an address symbol and
// no non-address ("lac [arg0]"), so T8d's suggestion keeps the symbol.
// Returns 1 with the spelling in bufP, 0 with bufP empty otherwise.
int
spellConstSymbol(OptWordP entryP, char *bufP, int size)
{
PNodeP innerP;
int syms;
int others;

    bufP[0] = '\0';

    if( !entryP || !(innerP = firstConstInner(entryP->exprP)) )
    {
        return(0);
    }

    syms = 0;
    others = 0;
    countConstLeaves(innerP, &syms, &others);

    if( !syms || others )
    {
        return(0);
    }

    if( !spellExpr(bufP, size, 0, innerP) )
    {
        bufP[0] = '\0';
        return(0);
    }

    return(1);
}

// Render a set of operate micro-ops as am1 source, in the order the hardware
// applies them.  An operate word with no micro-ops at all is nop.
void
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

// Render a skip group word from its bits: its conditions, then the switch and
// flag fields, then the i that reverses them.  For T2's inverted skip, a word
// with no source text yet.
void
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
