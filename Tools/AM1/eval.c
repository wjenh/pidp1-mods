/*
 * Support for evaluating various parse tree constructs.
*/
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "am1.h"
#include "y.tab.h"

int evalExpr(PNodeP);
int onesComplAdj(int);
int twosComplAdj(int);
int fixMinusZero(int val, int lhs, int rhs);
int countAscii(char *strP);
int countText(char *strP);

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

        lval = fixMinusZero(rslt, lval, rval);
        return( lval );

    case UNOP:
        switch( nodeP->value.ival )
        {
            case PARENS:
                return( _evalExpr(nodeP->rightP) );
                break;

            case UMINUS:
                lval = _evalExpr(nodeP->rightP);
                lval = ~lval;
                if( (lval == -1) && !keepMinusZero )
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
        return( (nodeP->value2.ival << 12) | nodeP->value.symP->value );
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
// which makes keeping the pc proper is not at all trivial.
long int
hashExpr(PNodeP nodeP)
{
long int lval, hilval;
long int rval, hirval;
long int partial;
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
        if( symP->flags & SYMF_RESOLVED )
        {
            return( symP->value );
        }
        else
        {
            // no value yet, use the symP as the 'value'
            uint64_t bigint = (uint64_t)symP;
            bigint = (bigint & 0xFFFFFFFF) << 18;  // mangle its bits 
            if( nodeP->type == BREF )
            {
                bigint += (0xEFE << 20);
            }

            return( (int)bigint );
        }

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
        if( ((signed int)i == -1) && !keepMinusZero )
        {
            i = 0;
        }

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
int
fixMinusZero(int val, int lhs, int rhs)
{
    if( !val && ((lhs < 0) || (rhs < 0)) && keepMinusZero )
    {
        return( -1 );
    }
    else
    {
        return( onesComplAdj(val) );
    }
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
countText(char *strP)
{
int i;
int rslt;

    i = strlen(strP);
    rslt = i / 3;
    if( i % 3 )             // not a full word
    {
        ++rslt;
    }

    return(rslt);
}
