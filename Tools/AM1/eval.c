/*
 * Support for evaluating various parse tree constructs.
*/
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>

#include "am1.h"
#include "y.tab.h"
#include "type340chars.h"

int evalExpr(PNodeP);
int onesComplAdj(int);
int twosComplAdj(int);
int fixMinusZero(int val);
int countAscii(char *strP);
int countText(FlexText flexText);
int type340Shift(char ch);
char asciiToType340(char ch);

extern bool keepMinusZero;
extern bool spaceIsAdd;
extern char processType340Escape(char ch);
extern void vwarn(int errtype, const char *msgP, ...);
extern void vwarnl(int errType, int lineno, const char *msgP, ...);

static int _evalExpr(PNodeP);
static bool hasIndirect(PNodeP nodeP);

// Set while poolKey() evaluates a constant, the code generators give the law
// range warning when they evaluate it, and a copy at parse time would repeat it.
static bool quietEval;

void verror(char *msgP, ...);
void verrorlf(int lineNo, const char *fileNameP, const char *msgP, ...);

// Evaluate an expression, mask to proper word size.
int
evalExpr(PNodeP nodeP)
{
    return( _evalExpr(nodeP) & WRDMASK );
}

// Evaluate an expression represented by the parse tree nodes passed.
// Although the math operations are done in 2's complement, the returned
// result will always be 1's complement.
int
_evalExpr(PNodeP nodeP)
{
int op;
int lval;
int rval;
int rslt;
SymNodeP symP;

    if( !nodeP )
    {
        return(0);
    }

    switch( nodeP->type )
    {
    case BINOP:
        // The returned values will be in 1's cmpl
        lval = _evalExpr(nodeP->leftP);
        rval = _evalExpr(nodeP->rightP);
        op = nodeP->value.ival;

        // bitwise operators, no -0 adjustment, and LAW special case
        switch( op )
        {
        case XOR:                   // MUST COME FIRST!
            lval = lval ^ rval;
            return( lval );

        case SEPARATOR:
            // See if someone did something silly like law -1
            // This is tricky, because 4096 is law i 0, so any value in 017777 is technically valid.
            // However, it would be very poor coding, reject it.
            // We have to explicitly check for an 'i' first.
            if( (nodeP->leftP->type == LAW) && !quietEval )
            {
                if( (unsigned)rval > 07777 )
                {
                    if(  hasIndirect(nodeP->rightP) )
                    {
                        if( (unsigned)(rval & ~010000) > 07777 )
                        {
                            vwarnl(WARN_LAW, nodeP->lineNo,
                                "law i used with a value not in the range 0 to 7777. This is almost certainly wrong!");
                        }
                    }
                    else
                    {
                        if( rval < 0 )
                        {
                            vwarnl(WARN_LAW, nodeP->lineNo,
                                "law used with a negative number. This is wrong, use 'law i nn'.");
                        }
                        else
                        {
                            vwarnl(WARN_LAW, nodeP->lineNo,
                                "law used with a value not in the range 0 to 7777. This is almost certainly wrong!");
                        }
                    }
                }
            }

            if( spaceIsAdd )
            {
                op = PLUS;      // fall thru to math code
            }
            else
            {
                lval = lval | rval;
                return( lval );
            }
            break;

        case OR:
            lval = lval | rval;
            return( lval );

        case AND:
            lval = lval & rval;
            return( lval );

        case LSHIFT:
            rslt = lval << twosComplAdj(rval);
            return( rslt );

        case RSHIFT:
            rslt = lval >> twosComplAdj(rval);
            return( rslt );
        }

        // Math operator, handle the one's cmpl adjustments
        lval = twosComplAdj(lval);
        rval = twosComplAdj(rval);
        switch( op )
        {
        case DIV:
            rslt = lval / rval;
            break;

        case MOD:
            rslt = lval % rval;
            break;

        case PLUS:
            rslt = lval + rval;
            break;

        case MINUS:
            rslt = lval - rval;
            break;

        case MUL:
            rslt = lval * rval;
            break;

        default:
            verror("unknown binary op %d in _evalExpr", nodeP->value.ival);
            // never returns, just to shut up overly-picky c compilers
            return(0);
        }

        // convert back to 1's complement, change -0 to 0 if needed
        lval = fixMinusZero(rslt);
        return( lval );

    case UNOP:
        switch( nodeP->value.ival )
        {
            case PARENS:
                return( _evalExpr(nodeP->rightP) );
                break;

            case UMINUS:
                lval = _evalExpr(nodeP->rightP);
                // Result is already 1's compl from _evalExpr()
                lval = ~lval;
                if( !keepMinusZero && (lval == -1) )
                {
                    lval = 0;
                }

                return(lval);

            case CMPL:
                return( ~_evalExpr(nodeP->rightP) );

        default:
            verror("unknown unary op %d in _evalExpr", nodeP->value.ival);
        }
        break;

    case CONSTANT:      // don't adjust
    case OPORABLE:
    case OPCODE:
    case OPADDR:
    case LAW:
        return( nodeP->value.symP->value );
        break;

    case DOT:
        return( nodeP->value.ival );   // also positive, don't adjust
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
        symP = nodeP->value.symP;
        if( !(symP->flags & SYMF_RESOLVED) )
        {
            // Name the reference's own line: by now the global lineno is the end
            // of the file.
            verrorlf(nodeP->srcLine, nodeP->srcFileP, "symbol %s in bank %d has no defined value",
                symP->name, symP->bank);
        }
        return( (nodeP->value2.ival << 12) | symP->value );
        break;

    case CHAR:
    case FLEXO:
    case LITCHAR:
        return( nodeP->value.ival );
        break;

    case INTEGER:
        return( nodeP->value.ival );
        break;

    case VALUESPEC:
    case IMOD:
        return( nodeP->value.symP->value );
        break;

    default:
        verror("unknown op %d, pc 0%04o in _evalExpr", nodeP->type, nodeP->pc);
    }

    // never reached: verror() does not return, and every other case returns
    return(0);
}

// Check to see if an i modifier occurs in the tree.
// Return true if so, else false
static bool
hasIndirect(PNodeP nodeP)
{
    if( !nodeP )
    {
        return(false);
    }

    if( nodeP->type == IMOD )
    {
        return(true);
    }
    else if( hasIndirect(nodeP->leftP) )
    {
        return(true);
    }
    else
    {
        return( hasIndirect(nodeP->rightP) );
    }
}

// Digest a symbol name into a 64-bit value.
// Used by hashExpr below for WILDREF (sym:*) references, which are resolved by name.
// Plain unresolved symbol references use the node's creation-order serial number instead,
// not this digest.
static unsigned long
hashName(const char *nameP)
{
unsigned long hashVal;

    hashVal = 0xCBF29CE484222325UL;          // FNV-1a offset basis

    while( *nameP )
    {
        hashVal ^= (unsigned long)(unsigned char)*nameP++;
        hashVal *= 0x100000001B3UL;          // FNV-1a prime
    }

    return( hashVal );
}

// This is used for constants to allow us to collapse equivalent expressions into one location.
// It wouldn't be needed except constants are allowed to reference currently-undefined symbols,
// which makes resolving expressions that result in the same value painful if the symbol is unresolved.
// As an expression is evaluated, if the values are known, that's what is used.
// If a symbol reference is encountered that hasn't been resolved, then it's serial number, assigned
// at symbol table create time is used to create a hash value, see below.
long int
hashExpr(PNodeP nodeP)
{
long int lval, hilval;
long int rval, hirval;
long int partial;
unsigned long bank;
unsigned long hashVal;
SymNodeP symP;

    if( !nodeP )
    {
        return(0);
    }

    switch( nodeP->type )
    {
    case SEPARATOR:
        return(0);

    case BINOP:
        lval = hashExpr(nodeP->leftP);
        rval = hashExpr(nodeP->rightP);
        hilval = lval & ~0777777;
        lval &= 0777777;
        hirval = rval & ~0777777;
        rval &= 0777777;

        switch( nodeP->value.ival )
        {
        case DIV:
            partial = onesComplAdj(twosComplAdj(lval) / twosComplAdj(rval));
            break;

        case MOD:
            partial = onesComplAdj(twosComplAdj(lval) % twosComplAdj(rval));
            break;

        case PLUS:
            partial = onesComplAdj(twosComplAdj(lval) + twosComplAdj(rval));
            break;

        case MINUS:
            partial = onesComplAdj(twosComplAdj(lval) - twosComplAdj(rval));
            break;

        case MUL:
            partial = onesComplAdj(twosComplAdj(lval) * twosComplAdj(rval));
            break;

        case AND:
            partial = lval & rval;
            break;

        case SEPARATOR:
        case OR:
            partial = lval | rval;
            break;

        case XOR:
            partial = lval ^ rval;
            break;

        case LSHIFT:
            partial = lval << twosComplAdj(rval);
            break;

        case RSHIFT:
            partial = lval >> twosComplAdj(rval);
            break;

        default:
            verror("unknown binary op %d in hashExpr", nodeP->value.ival);
            // never returns, just to shut up overly-picky c compilers
            return(0);
        }

        partial &= 0777777;

        if( hilval == hirval )
        {
            return( hilval | partial );
        }
        else if( !hilval || !hirval)
        {
            return( (hilval?hilval:hirval) | partial );
        }
        else
        {
            return( ((hilval << 8) + hirval) | partial );
        }

    case UNOP:
        rval = hashExpr(nodeP->rightP);
        switch( nodeP->value.ival )
        {
        case PARENS:
            return( rval );
        case UMINUS:
        case CMPL:
            lval = (~rval) & 0777777;
            return( (rval & ~0777777) | lval );
        case LAW:
            return( nodeP->value.symP->value );
        default:
            verror("unknown unary op %d in hashExpr", nodeP->value.ival);
            // never returns, just to shut up overly-picky c compilers
            return(0);
        }

    case CONSTANT:
        return( hashExpr(nodeP->rightP) );

    case DOT:
        return( nodeP->value.ival );

    case OPORABLE:
    case OPCODE:
    case OPADDR:
    case VALUESPEC:
    case IMOD:
        return( nodeP->value.symP->value );

    case ADDR:
    case LCLADDR:
    case BREF:
        symP = nodeP->value.symP;

        // The value is always a memory address, but be sure it has the correct bank address
        // If it's a bref, we want a full 16 bit address.
        if( nodeP->type == BREF )
        {
            bank = nodeP->value2.ival;
        }
        else
        {
            bank = 0;       // unqualified addresses always appear to be in bank 0.
        }

        if( symP->flags & SYMF_RESOLVED )
        {
            lval = (bank << 12) | (symP->value & 07777);
            return( lval );
        }

        // Use the symbol node's creation-order serial number as the 'value', modified if there was an
        // explicit bank ref.
        // Since its true value isn't known, we don't want it to be confused
        // with an actual memory address, all values except this one have resolved to an 18-bit
        // number, so the serial number is shifted up 22 bits, well clear of that range, and an explicit
        // bank ref lands in the high 4 bits.
        //
        // This isn't perfect: different unresolved symbols that finally resolve to the same
        // address won't hash together, but that only means an extra word of memory will be used.
        hashVal = (unsigned long)symP->serialNumber;
        hashVal = ((hashVal & 0xFFFFFFFFF) << 22) | (bank << 60);
        return( (long)hashVal );

    case WILDREF:
        // All we have is the symbol NAME -- but for a wildcard ref that's exactly right:
        // sym:* is resolved BY NAME, so equal names mean the same eventual resolution and
        // deserve the same fingerprint. Digest the name (deterministic, unlike the string's
        // malloc address).  A wildcard names no bank, so its bank bits are 0.
        bank = 0;
        hashVal = hashName(nodeP->value.strP);
        hashVal = ((hashVal & 0xFFFFFFFFF) << 22) | (bank << 60);
        return( (long)hashVal );

    case INTEGER:
    case CHAR:
    case FLEXO:
    case LITCHAR:
        return( nodeP->value.ival );

    default:
        verror("unknown op %d in hashExpr", nodeP->type);
        // never returns, just to shut up overly-picky c compilers
        return(0);
    }
}

// A pool key's text as poolKey() builds it, grown as needed.
typedef struct
{
    char *bufP;
    size_t len;
    size_t size;
} KeyText;

static void appendExprText(KeyText *textP, PNodeP nodeP);

// Append printf-style text to a key.
static void
keyAppend(KeyText *textP, const char *fmtP, ...)
{
va_list argP;
int need;

    for( ;; )
    {
        va_start(argP, fmtP);
        need = vsnprintf(textP->bufP + textP->len, textP->size - textP->len, fmtP, argP);
        va_end(argP);

        if( need < 0 )
        {
            verror("can't format a constant's pool key");
        }

        if( (size_t)need < (textP->size - textP->len) )
        {
            textP->len += need;
            return;
        }

        textP->size = (textP->size * 2) + need;

        if( !(textP->bufP = realloc(textP->bufP, textP->size)) )
        {
            verror("out of memory for a constant's pool key");
        }
    }
}

// Start an empty key.
static void
keyStart(KeyText *textP)
{
    textP->size = 64;
    textP->len = 0;

    if( !(textP->bufP = malloc(textP->size)) )
    {
        verror("out of memory for a constant's pool key");
    }

    textP->bufP[0] = 0;
}

// Whether an expression reads something whose value the parse does not know yet:
// a symbol not yet defined, a sym:* reference, or a constant, whose value is its
// slot's address.
static bool
readsUnknown(PNodeP nodeP)
{
    if( !nodeP )
    {
        return(false);
    }

    switch( nodeP->type )
    {
    case ADDR:
    case LCLADDR:
    case BREF:
        return( !(nodeP->value.symP->flags & SYMF_RESOLVED) );

    case WILDREF:
    case CONSTANT:
        return(true);

    case BINOP:
        return( readsUnknown(nodeP->leftP) || readsUnknown(nodeP->rightP) );

    case UNOP:
        return( readsUnknown(nodeP->rightP) );

    default:
        return(false);
    }
}

// An expression's canonical text, allocated; the caller frees it.
static char *
exprText(PNodeP nodeP)
{
KeyText text;

    keyStart(&text);
    appendExprText(&text, nodeP);
    return(text.bufP);
}

// Append an expression's canonical text: what it reads, operator by operator.
// A part that reads nothing unknown is written as its value, so equal parts match
// however they are spelled.  The operands of an operator that commutes are written
// in a fixed order, so [1+foo] and [foo+1] still share a slot.
static void
appendExprText(KeyText *textP, PNodeP nodeP)
{
char *leftP;
char *rightP;
char *swapP;
const char *opP;
int op;

    if( !readsUnknown(nodeP) )
    {
        keyAppend(textP, "%lo", (unsigned long)hashExpr(nodeP));
        return;
    }

    switch( nodeP->type )
    {
    case BINOP:
        op = nodeP->value.ival;
        leftP = exprText(nodeP->leftP);
        rightP = exprText(nodeP->rightP);

        switch( op )
        {
        case PLUS:
            opP = "+";
            break;
        case MINUS:
            opP = "-";
            break;
        case MUL:
            opP = "*";
            break;
        case DIV:
            opP = "/";
            break;
        case MOD:
            opP = "%";
            break;
        case AND:
            opP = "&";
            break;
        case OR:
            opP = "|";
            break;
        case XOR:
            opP = "^";
            break;
        case LSHIFT:
            opP = "<<";
            break;
        case RSHIFT:
            opP = ">>";
            break;
        case SEPARATOR:
            opP = "_";
            break;
        default:
            verror("unknown binary op %d in a constant's pool key", op);
            return;
        }

        if( ((op == PLUS) || (op == MUL) || (op == AND) || (op == OR) || (op == XOR) || (op == SEPARATOR)) &&
            (strcmp(leftP, rightP) > 0) )
        {
            swapP = leftP;
            leftP = rightP;
            rightP = swapP;
        }

        keyAppend(textP, "(%s %s %s)", leftP, opP, rightP);
        free(leftP);
        free(rightP);
        return;

    case UNOP:
        switch( nodeP->value.ival )
        {
        case PARENS:
            break;
        case UMINUS:
            keyAppend(textP, "-");
            break;
        case CMPL:
            keyAppend(textP, "~");
            break;
        default:
            verror("unknown unary op %d in a constant's pool key", nodeP->value.ival);
            return;
        }

        appendExprText(textP, nodeP->rightP);
        return;

    case CONSTANT:
        // A constant inside a constant: its own slot's key names it.
        keyAppend(textP, "[%s]", nodeP->value.symP->name);
        return;

    default:
        // A symbol not yet defined, or a sym:* reference: hashExpr() gives what
        // identifies it, the symbol's serial number and bank, or the name digested.
        keyAppend(textP, "s%lx", (unsigned long)hashExpr(nodeP));
        return;
    }
}

// The constant pool's key for an expressions.
// A bank's literals with equal keys share/ one slot which the parser places
// before it knows the slot's value.
// The key is generated by hashExpr() when the expression applies an operator to
// something the parse has not seen yet or is a constant inside a constant, '#' and
// the expression's canonical text.
// The pool is a tree ordered by the key's text, placed in pre-order.
// '#' sorts below/ every digit so a key with the text sorts against every other key exactly as its
// number alone does.
// The pool, and so every address, changes only where two literals that shared a slot now differ.
// '!' sorts below every character the canonical text can start with,
// so a key with the value sorts the same way against a key with the text and the same number.
// Returns the key, allocated; the caller frees it.
char *
poolKey(PNodeP exprP)
{
KeyText text;
PNodeP nodeP;
long number;
int value;

    keyStart(&text);
    number = hashExpr(exprP);
    keyAppend(&text, "%ld", number);

    for( nodeP = exprP; nodeP && (nodeP->type == UNOP) && (nodeP->value.ival == PARENS); nodeP = nodeP->rightP )
    {
    }

    if( nodeP && readsUnknown(nodeP) &&
        ((nodeP->type == BINOP) || (nodeP->type == UNOP) || (nodeP->type == CONSTANT)) )
    {
        keyAppend(&text, "#");
        appendExprText(&text, nodeP);
    }
    else if( nodeP && !readsUnknown(nodeP) )
    {
        quietEval = true;
        value = evalExpr(exprP);
        quietEval = false;

        if( (long)value != (number & WRDMASK) )
        {
            keyAppend(&text, "#!%o", value);
        }
    }

    return(text.bufP);
}

// Given a number in 2s cmpl, convert to 1s cmpl.
// Only affects negative numbers.
int
onesComplAdj(int oc)
{
unsigned int i;

    i = (unsigned int)oc;

    if( oc < 0 )
    {
        i--;
        oc = (signed int)i;
    }

    return(oc);
}

// Given a number in 1s cmpl, convert to 2s cmpl.
// Only affects negative numbers.
int
twosComplAdj(int oc)
{
unsigned int i;

    i = (unsigned int)oc;

    if( oc < 0 )
    {
        i++;
        oc = (signed int)i;
    }

    return(oc);
}

// Handle -0 results, result is 1s cmpl
// The value is 2's complement.
// Convert to 1's complement.
// If not keeping -0, adjust to 0.
int
fixMinusZero(int val)
{
    val = onesComplAdj(val);
    if( !keepMinusZero && (val == -1) )
    {
        val = 0;
    }

    return( val );
}

// Count packed ascii, return number of words needed
// Could be a macro, not much to it.
int
countAscii(char *strP)
{
int chars;
        
    chars = strlen(strP);
    return( (chars / 2) + 1);
}

// Count packed flexo code
int
countText(FlexText flexText)
{
int i;
int rslt;

    i = flexText.nchars;
    rslt = i / 3;
    if( i % 3 )             // not a full word
    {
        ++rslt;
    }

    return(rslt);
}

// Convert an ascii string to Type 340 characters.
// Backslash escapes are processed separately.
// Unlike flex/concise characters, these collate nicely in semi-ascii sequence, so no lookup tables needed.
char
asciiToType340(char ch)
{
    // Map lower case to upper case
    if( (ch >= 'a') && (ch <= 'z') )
    {
        ch = ch - 'a' + 'A'; 
    }

    if( (ch >= '@') && (ch <= 'Z') )
    {
        ch = ch - '@';
    }
    else if( (ch >= ' ') && (ch <= '?') )
    {
        ;   // keep as is
    }
    else
    {
        // Ok, a pain
        switch( ch )
        {
        case '~':
            ch = 043;
            break;
        case '\\':
            ch = 052;
            break;
        case '[':
            ch = 053;
            break;
        case ']':
            ch = 054;
            break;
        case '{':
            ch = 055;
            break;
        case '}':
            ch = 056;
            break;
        case '_':
            ch = 060;
            break;
        case '|':
            ch = 062;
            break;
        case '`':
            ch = 066;
            break;
        case '^':
            ch = 067;
            break;
        default:
            ch = 0; // a blob
        }
    }

    return( ch );
}

// Check an ascii character, determine if it needs upper or lower shift when converted to a Type 340 character.
// Returns 1 if needs upper, -1 if needs lower, 0 if unknown.
int
type340Shift(char ch)
{
    if( islower(ch) )
    {
        return(-1);
    }
    else if( isupper(ch) )
    {
        return(1);
    }
    else if( isdigit(ch) )
    {
        return(1);
    }
    else if( strchr("!\"#$%&'()*+,-./:;<=>?", ch) )    // character set 1
    {
        return(1);
    }
    else if( strchr("~\\[]{}_|`^", ch) )    // character set 2
    {
        return(-1);
    }

    // Not categorized
    return(0);
}
