/*
 * Process a parse tree to generate macro source output.
*/
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "am1.h"
#include "y.tab.h"

#define LOADER_HALT 07772       // the halt instruction in our loader, keep in sync with xldr

extern SymListP constsListP;
extern bool sawBank;
extern BankContextP banksP;
extern bool noWarn;

extern int countText(char *strP);
extern int countAscii(char *strP);
extern int evalExpr(PNodeP);
extern int onesComplAdj(int);

static void emitStatements(FILE *, PNodeP);
static void emitOperand(FILE *, PNodeP);
static void emitAscii(FILE *outfP, char *strP);
static void emitText(FILE *outfP, char *strP);
static void emitVars(FILE *outfP, PNodeP nodeP);
static void emitConstants(FILE *outfP, SymNodeP nodeP);

void verror(char *msgP, ...);

// Walk a tree and emit equivalent macro1 code.
void
macCodegen(FILE *outfP, PNodeP rootP)
{
    // The root is a HEADER.
    // The root lhs is the program body, the rhs the START or PAUSE at the end of the program.
    fprintf(outfP,"%s\n", rootP->value.strP);
    emitStatements(outfP, rootP->leftP);

    // Finish any trailing constants
    for(BankContextP bankP = banksP; bankP; bankP = bankP->nextP)
    {
        if( bankP->constSymP )
        {
            fprintf(outfP, "/ Constants for bank %d\n", bankP->bank);
            fprintf(outfP, "%o/\n", bankP->cur_pc);
            emitConstants(outfP, bankP->constSymP);
        }
    }

    if( rootP->rightP->type == PAUSE )
    {
        if( !noWarn )
        {
            fprintf(stderr,"am1: WARNING: pause is not supported by macro1, replacing with a jump to hlt\n");
        }

        fprintf(outfP,"start %o\n", LOADER_HALT);
    }
    else
    {
        fprintf(outfP,"start %o\n", rootP->rightP->value.ival);
    }
}

static void
emitStatements(FILE *outfP, PNodeP nodeP)
{
int i, j;
PNodeP node2P;
char str[128];

    while( nodeP )
    {
        switch( nodeP->type )
        {
        case COMMENT:
            if( strlen(nodeP->value.strP) > 70 )
            {
                // macro screws up on long lines
                strncpy(str, nodeP->value.strP, 70);
                str[71] = 0;
                fprintf(outfP, "/ %s\n", str);
                fprintf(outfP, "/ %s\n", nodeP->value.strP + 70);
            }
            else
            {
                fprintf(outfP, "/ %s\n", nodeP->value.strP);
            }
            break;

        case ORIGIN:
            fprintf(outfP, "%o/", nodeP->value.ival);
            if( nodeP->rightP )
            {
                fprintf(outfP," ");
                emitOperand(outfP, nodeP->rightP);
            }
            break;

        case EXPR:
            fprintf(outfP, "    ");
            emitOperand(outfP, nodeP->rightP);
            break;

        case LOCATION:
            fprintf(outfP, "%s,", nodeP->value.symP->name);
            if( nodeP->rightP )
            {
                fprintf(outfP," ");
                emitOperand(outfP, nodeP->rightP);
            }
            fprintf(outfP,"\n");
            break;

        case LCLLOCATION:
            if( nodeP->rightP )
            {
                fprintf(outfP, "    ");
                emitOperand(outfP, nodeP->rightP);
                fprintf(outfP,"\n");
            }
            break;

        case VARS:
            fprintf(outfP,"/ variables\n");
            emitVars(outfP, nodeP->rightP);
            break;

        case BANK:
            fprintf(outfP,"/ BANK - following is in bank %d\n", nodeP->value.ival );
            break;

        case CONSTANTS:
            fprintf(outfP,"/ constants\n");
            emitConstants(outfP, nodeP->value.symP);
            break;

        case TEXT:
            fprintf(outfP, "/ Text table\n");
            emitText(outfP, nodeP->value.strP);
            fprintf(outfP, "/ End\n");
            break;

        case ASCII:
            fprintf(outfP, "/ Ascii table\n");
            emitAscii(outfP, nodeP->value.strP);
            fprintf(outfP, "/ End\n");
            break;

        case TABLE:
            fprintf(outfP, "/ Data table\n");

            if( nodeP->rightP )     // has initializer
            {
                j = evalExpr(nodeP->rightP);

                for( i = 0; i < nodeP->value.ival; ++i )
                {
                    nodeP->pc++;
                    fprintf(outfP, "    %o\n", j);
                }
            }
            else
            {
                fprintf(outfP, ".+%o/\n", nodeP->value.ival);
            }

            fprintf(outfP, "/ End\n");
            break;

        case TERMINATOR:
            fprintf(outfP, "\n");
            break;

        default:
            // just ignore
            break;
        }

        nodeP = nodeP->leftP;
    }
}

// Very similar to evalExpr(), but doesn't do full reduction
static void
emitOperand(FILE *outfP, PNodeP nodeP)
{
int lval;
int rval;
char ch;
SymNodeP symP;
PNodeP node2P;

    if( !nodeP )
    {
        return;
    }

    switch( nodeP->type )
    {
    case BINOP:
        if( nodeP->value.ival == XOR )
        {
            // there is no xor in macro1, have to reduce everything
            lval = onesComplAdj(evalExpr(nodeP->leftP));
            rval = onesComplAdj(evalExpr(nodeP->rightP));
            fprintf(outfP,"%o", (lval ^ rval) & WRDMASK);
        }
        else if( nodeP->value.ival == DIV )
        {
            // there is no divide in macro1, have to reduce everything
            lval = evalExpr(nodeP->leftP);
            rval = evalExpr(nodeP->rightP);
            fprintf(outfP,"%o", onesComplAdj(lval / rval) & WRDMASK);
        }
        else if( nodeP->value.ival == MOD )
        {
            // there is no mod in macro1, have to reduce everything
            lval = onesComplAdj(evalExpr(nodeP->leftP));
            rval = onesComplAdj(evalExpr(nodeP->rightP));
            fprintf(outfP,"%o", (lval ^ rval) & WRDMASK);
        }
        else
        {
            emitOperand(outfP, nodeP->leftP);

            switch( nodeP->value.ival )
            {
            case PLUS:
                ch = '+';
                break;
            case MINUS:
                ch = '-';
                break;
            case MUL:
                ch = '*';
                break;
            case AND:
                ch = ' ';
                break;
            case OR:
                ch = '!';
                break;
            case SEPARATOR:
                ch = ' ';
                break;
            default:
                verror("unknown binary op %d in emitOperand", nodeP->value.ival);
            }

            fprintf(outfP,"%c", ch);
            emitOperand(outfP, nodeP->rightP);
        }
        break;

    case UNOP:
        switch( nodeP->value.ival )
        {
        case PARENS:
            emitOperand(outfP, nodeP->rightP);
            break;
        case UMINUS:
            rval = onesComplAdj(evalExpr(nodeP));
            fprintf(outfP, "%o", (rval) & WRDMASK);
            break;
        case CMPL:
            // there is no complement in macro1, have to reduce
            rval = onesComplAdj(evalExpr(nodeP));
            fprintf(outfP, "%o", (rval) & WRDMASK);
            break;
        default:
            verror("unknown unary op %d in emitOperand", nodeP->value.ival);
        }
        break;

    case CONSTANT:
        fprintf(outfP,"%04o",nodeP->value.symP->value);
        break;

    case DOT:
        fprintf(outfP,".");
        break;

    case OPORABLE:
    case OPCODE:
    case OPADDR:
        fprintf(outfP,"%s", nodeP->value.symP->name );
        break;

    case BREF:
        fprintf(outfP,"BREF %s:%d", nodeP->value.symP->name, nodeP->value2.ival );
        break;

    case LCLADDR:
        // local addrs are relative to the current location unless they were resolved as global
        symP = nodeP->value.symP;
        if( symP->flags & SYMF_RESOLVED )
        {
            if( (symP->flags & SYM_MASK) == SYM_GLOB )
            {
                fprintf(outfP, "%s", symP->name);
            }
            else
            {
                lval = symP->value;
                rval = nodeP->pc;
                if( lval >= rval )
                {
                    fprintf(outfP, ".+%o", lval - rval );
                }
                else
                {
                    fprintf(outfP, ".-%o", rval - lval );
                }
            }
            break;
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
            fprintf(outfP, "%s", symP->name );
        }
        else
        {
            verror("symbol %s has no defined value", symP->name);
        }
        break;

    case LITCHAR:
    case CHAR:
    case FLEXO:
        fprintf(outfP, "%06o", nodeP->value.ival & WRDMASK);
        break;

    case INTEGER:
        fprintf(outfP, "%o", nodeP->value.ival & WRDMASK);
        break;

    case VALUESPEC:
        fprintf(outfP, "%s", nodeP->value.symP->name);
        break;

    case LOCAL:
    case ENDLOC:
    case FORCELOC:
        break;      // ignore

    case TERMINATOR:
        fprintf(outfP, "\n");
        break;

    default:
        verror("unknown op %d in emitOperand", nodeP->type);
    }
}

// Emit packed ascii
static void
emitAscii(FILE *outfP, char *strP)
{
int i;

    for( i = 0; *strP != 0; i ^= 1 )
    {
        if( !i )
        {
            fprintf(outfP, "    ");
        }

        fprintf(outfP, "%03o", *strP++);

        if( i )
        {
            fprintf(outfP, "\n");
        }
    }

    if( i )
    {
        fprintf(outfP, "000\n");
    }
    else
    {
        fprintf(outfP, "    000000\n");
    }
}

// Emit packed flexo code
static void
emitText(FILE *outfP, char *strP)
{
int i;
int val;

    for( val = i = 0; *strP != 0; i++ )
    {
        if( i && !(i % 3) )
        {
            fprintf(outfP, "    %06o\n", val);
            val = 0;
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
        fprintf(outfP, "    %06o\n", val);
    }
    else if( !*strP && !( i % 3) )
    {
        fprintf(outfP, "    %06o\n", val);      // didn't emit the word yet
    }
}

// Walk a list of variables, emit the storage
static void
emitVars(FILE *fP, PNodeP nodeP)
{
SymNodeP symP;

    while( nodeP )
    {
        symP = nodeP->value.symP;
        fprintf(fP,"%s, %06o\n", symP->name, (nodeP->leftP)?evalExpr(nodeP->leftP):0);
        nodeP = nodeP->rightP;
    }
}

// Walk a symbol table of constants, emit the values
static void
emitConstants(FILE *fP, SymNodeP symP)
{
    if( !symP )
    {
        return;
    }

    fprintf(fP,"    %06o\n", symP->value2);

    emitConstants(fP, symP->leftP);
    emitConstants(fP, symP->rightP);
}
