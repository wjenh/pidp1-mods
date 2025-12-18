/*
 * Support for evaluating various parse tree constructs.
*/
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "am1.h"
#include "y.tab.h"

int evalExpr(PNodeP);
int onesComplAdj(int);
int twosComplAdj(int);
int countAscii(char *strP);
int countText(char *strP);

void verror(char *msgP, ...);

int
evalExpr(PNodeP nodeP)
{
int lval;
int rval;
SymNodeP symP;

    if( !nodeP )
    {
        return(0);
    }

    switch( nodeP->type )
    {
    case BINOP:
        lval = evalExpr(nodeP->leftP);
        rval = evalExpr(nodeP->rightP);

        switch( nodeP->value.ival )
        {
        case SEPARATOR:     // the default behavior from macro1 is add
        case PLUS:
            return( lval + rval );
        case MINUS:
            return( lval - rval );
        case MUL:
            return( lval * rval );
        case DIV:
            return( lval / rval );
        case MOD:
            return( lval % rval );
        // The bitwise oprs need to use 1's cmpl
        case AND:
            lval = onesComplAdj(lval);
            rval = onesComplAdj(rval);
            return( lval & rval );
        case OR:
            lval = onesComplAdj(lval);
            rval = onesComplAdj(rval);
            return( lval | rval );
        case XOR:
            lval = onesComplAdj(lval);
            rval = onesComplAdj(rval);
            return( lval ^ rval );
        default:
            verror("unknown binary op %d in evalExpr", nodeP->value.ival);
        }

    case UNOP:
        rval = evalExpr(nodeP->rightP);
        switch( nodeP->value.ival )
        {
        case PARENS:
            return( rval );
        case UMINUS:
            return( -rval );
        case CMPL:
            return( ~onesComplAdj(rval) );
        default:
            verror("unknown unary op %d in evalExpr", nodeP->value.ival);
        }

    case CONSTANT:
        symP = nodeP->value.symP;
        if( !(symP->flags & SYMF_RESOLVED) )
        {
            symP->flags |= SYMF_RESOLVED;
            symP->value2 = evalExpr(nodeP->rightP);
        }

        return( symP->value );

    case DOT:
        return( nodeP->value.ival );

    case OPORABLE:
    case OPCODE:
    case OPADDR:
        return( nodeP->value.symP->value );

    case ADDR:
    case LCLADDR:
    case BREF:
        symP = nodeP->value.symP;
        if( symP->flags & SYMF_RESOLVED )
        {
            rval = symP->value;
        }
        else
        {
            verror("symbol %s has no defined value", symP->name);
        }

        if( nodeP->type == BREF )
        {
            rval = (nodeP->value2.ival << 12) + rval;
        }
        return( rval );


    case INTEGER:
    case CHAR:
    case FLEXO:
    case LITCHAR:
        return( nodeP->value.ival );

    default:
        verror("unknown op %d in evalExpr", nodeP->type);
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
        case PLUS:
        case SEPARATOR:
            partial = lval + rval;
            break;
        case MINUS:
            partial = lval - rval;
            break;
        case MUL:
            partial = lval * rval;
            break;
        case DIV:
            partial = lval / rval;
            break;
        case MOD:
            partial = lval ^ rval;
            break;
        case AND:
            partial = lval & rval;
            break;
        case OR:
            partial = lval | rval;
            break;
        case XOR:
            partial = lval ^ rval;
            break;
        default:
            verror("unknown binary op %d in hashExpr", nodeP->value.ival);
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
            return( -rval );
        case CMPL:
            return( ~rval );
        default:
            verror("unknown unary op %d in evalExpr", nodeP->value.ival);
        }

    case CONSTANT:
        return( hashExpr(nodeP->rightP) );

    case DOT:
        return( nodeP->value.ival );

    case OPORABLE:
    case OPCODE:
    case OPADDR:
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
        if( (signed int)i == -1 )
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

// Count packed ascii, return number of words needed
// Could be a macro, not much to it.
int
countAscii(char *strP)
{
int words;
        
    if( !((words = strlen(strP)) & 1) )
    {
        ++words;
    }

    return( words );
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
