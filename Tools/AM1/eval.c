/*
 * Support for evaluating various parse tree constructs.
 *
 * 29-Apr-2026 wje - add type340 support
*/
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "am1.h"
#include "y.tab.h"
#include "type340chars.h"

int evalExpr(PNodeP);
int onesComplAdj(int);
int twosComplAdj(int);
int fixMinusZero(int val);
int countAscii(char *strP);
int countText(FlexText flexText);
int countType340(char *t340StrP);
char processType340Escape(char ch);
char asciiToType340(char ch);

extern bool keepMinusZero;
extern bool spaceIsAdd;

static int _evalExpr(PNodeP);

void verror(char *msgP, ...);

// Evaluate an expression, mask to proper word size.
int
evalExpr(PNodeP nodeP)
{
    return( _evalExpr(nodeP) & WRDMASK );
}

// Evaluate an expression represnted by the parse tree nodes passed.
// Although the math operations are done in 2's complement, the returned
// result will always be 1's complement.
int
_evalExpr(PNodeP nodeP)
{
int op;
int lval;
int rval;
int rslt;
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
        lval = _evalExpr(nodeP->leftP);
        rval = _evalExpr(nodeP->rightP);

        op = nodeP->value.ival;

        // bitwise operators, no -0 adjustment
        switch( op )
        {
        case XOR:
            lval = lval ^ rval;
            return( lval );

        case SEPARATOR:
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
                break;

            case CMPL:
                return( ~_evalExpr(nodeP->rightP) );
                break;

        default:
            verror("unknown unary op %d in _evalExpr", nodeP->value.ival);
        }
        break;

    case CONSTANT:      // don't adjust
    case OPORABLE:
    case OPCODE:
    case OPADDR:
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
            verror("symbol %s in bank %d has no defined value", symP->name, symP->bank);
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
        return( nodeP->value.symP->value );
        break;

    default:
        verror("unknown op %d, pc 0%04o in _evalExpr", nodeP->type, nodeP->pc);
    }
}

// This is used for constants to allow us to collapse equivalent expressions.
// It wouldn't be needed except constants are allowed to reference currently-undefined symbols,
// which makes resolving expressions that result in the same value painful if the symbol is unresolved.
// As an expression is evaluated, if the values are known, that's what is used.
// If a symbol reference is encountered that hasn't been resolved, then the address of the symbol in
// the symbol table is used to create a hash value, see below.
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
        lval &= 0777777;

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

        // Use the symP as the 'value', modify it if there was an explicit bank ref.
        // But, since its true value isn't known, we don't want it to be confused with an actual memory address.
        // All values except this one have resolved to an 18-bit number, which is the final value.
        // So, scrabmle the symbol table address, take the low 38 bits, shift it up 22 bits.
        // Why 22?
        // The address will have been from malloc and will be unique within the lower sizeof(Symbol) bits.
        // For safety, shift the address up 8 bits after taking the low 36 bits.
        // If it's a bank ref, it's an explicit reference, add in the bank as the high 4 bits.
        // This isn't perfect, different unresolved symbols that finally resolve to the same address
        // won't hash together, but that only means an extra word of memory will be used.
        hashVal = (unsigned long)symP;
        hashVal = ((hashVal & 0xFFFFFFFFF) << 22) | (bank << 60);
        return( (long)hashVal );

    case WILDREF:
        // All we have is the symbolname, use the string address mangled similarly as above.
        hashVal = (unsigned long)(nodeP->value.strP);
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

// This is similar to countText() in that characters are packed 3 to an 18-bit word.
// The passed string must have already been converted to a TYPE 340 character set string.
// Note that 0 is NOT the end of a Type 340 string, the TYPE340END character is.
int
countType340(char *strP)
{
int rslt;
int count;

    for( count = 0;; )
    {
        ++count;
        if( *strP++ == TYPE340END )
        {
            break;
        }
    }

    // Now have the character count.
    // If it isn't a multiple of 3, we have an incomplete word.
    rslt = count / 3;
    if( count % 3 )
    {
        ++rslt;
    }

    return(rslt);
}

// Handle the backslash-x conversions for type 340 characters.
// Return the character or 0 if it was a line continuation.
char
processType340Escape(char ch)
{
    switch( ch )
    {
    case '\n':  	// backslash-<newline> ignored, it's a line continuation
        return(0);

    case 'e':
        ch = TYPE340END;    // an explicit end marker, we're done
        break;

    case 'U':
        ch = TYPE340UPPER;  // upper shift
        break;

    case 'L':
        ch = TYPE340LOWER;  // lower shift
        break;

    case 'b':		    // blob character
        ch = TYPE340BLOB;
        break;

    case 'n':
        ch = TYPE340NL;     // newline, a line feed and cr, special case
        break;

    case 'l':
        ch = TYPE340LF;     // newline, a line feed
        break;

    case 'r':		    // a carriage return
        ch = TYPE340CR;
        break;

    default:		    // not supported
        ch = TYPE340BLOB;
        break;
    }

    return(ch);
}

// Convert an ascii string to Type 340 characters.
// Backslash escapes are processed separately.
// Unlike flex/concise characters, these collate nicely in semi-ascii seqence, so no lookup tables needed.
// Lowercase alpha characters will be mapped to their uppercase equivalents.
char
asciiToType340(char ch)
{
    // Map lower case to upper case
    if( (ch >= 'a') && (ch <= 'z') )
    {
        ch = ch - 'a' + 'A'; 
    }

    if( (ch < ' ') || (ch > 'Z') )
    {
        ch = TYPE340BLOB;
    }
    else if( (ch >= 'A') && (ch <= 'Z') )
    {
        ch = ch - 'A' + 1;
    }

    // If not matched above, char is the same
    return( ch );
}
