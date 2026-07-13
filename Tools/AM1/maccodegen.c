/*
 * Process a parse tree to generate macro source output.
*/
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <stdbool.h>

#include "am1.h"
#include "y.tab.h"
#include "type340chars.h"

#define LOADER_HALT 07772       // the halt instruction in our loader, keep in sync with xldr

extern bool sawBank;
extern BankContextP banksP;

extern bool doWarn(int warnNo);
extern int evalExpr(PNodeP);
extern int onesComplAdj(int);

static void emitStatements(FILE *, PNodeP);
static void emitOperand(FILE *, PNodeP);
static void emitAscii(FILE *outfP, char *strP);
static void emitText(FILE *outfP, FlexText flexText);
static void emitVars(FILE *outfP, PNodeListP listP);
static int emitConstants(FILE *outfP, SymNodeP nodeP);

void verror(char *msgP, ...);

// Walk a tree and emit equivalent macro1 code.
void
macCodegen(FILE *outfP, PNodeP rootP)
{
    // The root is a HEADER.
    // The root lhs is the program body, the rhs the START or STOP at the end of the program.
    fprintf(outfP,"%s\n", rootP->value.strP);
    emitStatements(outfP, rootP->leftP);

    // Finish any trailing constants and vars
    for(BankContextP bankP = banksP; bankP; bankP = bankP->nextP)
    {
        if( bankP->constSymP )
        {
            fprintf(outfP, "/ Constants for bank %d\n", bankP->bank);
            fprintf(outfP, "%o/\n", bankP->cur_pc);
            bankP->cur_pc += emitConstants(outfP, bankP->constSymP);
        }

        if( bankP->varNodesP )
        {
            fprintf(outfP, "/ Vars for bank %d\n", bankP->bank);
            fprintf(outfP, "%o/\n", bankP->cur_pc);
            emitVars(outfP, bankP->varNodesP);
        }
    }

    if( rootP->rightP->type == STOP )
    {
        if( doWarn(WARN_STOP) )
        {
            fprintf(stderr,"am1: WARNING: stop is not supported by macro1, replacing with a jump to hlt\n");
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
bool noNl = false;
bool sawTerm = true;    // because we just output the header, which terminated its line
char *cP;
char str[128];

    while( nodeP )
    {
        switch( nodeP->type )
        {
        case COMMENT:
            if( !sawTerm )
            {
                // silly macro needs a tab
                fprintf(outfP,"\t");
            }

            cP = nodeP->value.strP;
            while( isspace(*cP) )
            {
                ++cP;           // dump leading spaces
            }

            if( strlen(cP) > 70 )
            {
                // macro screws up on long lines
                strncpy(str, cP, 70);
                cP[71] = 0;
                fprintf(outfP, "/ %s\n", str);
                fprintf(outfP, "/ %s\n", cP + 70);
            }
            else
            {
                fprintf(outfP, "/ %s\n", cP);
            }
            break;

        case ORIGIN:
            fprintf(outfP, "%o/", nodeP->value.ival);
            if( nodeP->rightP )
            {
                fprintf(outfP," ");
                emitOperand(outfP, nodeP->rightP);
            }
            else
            {
                fprintf(outfP, "\n");
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
                // TEXT/ASCII/TYPE340 right children come from labelTrailer
                // matching a text directive on the same line as the label.
                // They cannot be passed to emitOperand, so dispatch to the
                // appropriate text-emit function instead.
                switch( nodeP->rightP->type )
                {
                case TEXT:
                case TYPE340:
                    emitText(outfP, nodeP->rightP->value.flexText);
                    break;

                case ASCII:
                    emitAscii(outfP, nodeP->rightP->value.strP);
                    break;

                default:
                    emitOperand(outfP, nodeP->rightP);
                    break;
                }
            }
            break;

        case LCLLOCATION:
            if( nodeP->rightP )
            {
                fprintf(outfP, "    ");
                // Same TEXT/ASCII/TYPE340 handling as LOCATION above.
                switch( nodeP->rightP->type )
                {
                case TEXT:
                case TYPE340:
                    emitText(outfP, nodeP->rightP->value.flexText);
                    break;

                case ASCII:
                    emitAscii(outfP, nodeP->rightP->value.strP);
                    break;

                default:
                    emitOperand(outfP, nodeP->rightP);
                    break;
                }
            }
            break;

        case VARS:
            fprintf(outfP,"/ variables\n");
            emitVars(outfP, (PNodeListP)(nodeP->value.ptr));
            break;

        case BANK:
            if( doWarn(WARN_BANK) )
            {
                fprintf(stderr,"am1: WARNING: banks are not supported by macro1!\n");
            }

            fprintf(outfP,"/ BANK - following is in bank %d\n", nodeP->value.ival );
            noNl = true;
            break;

        case CONSTANTS:
            fprintf(outfP,"/ constants\n");
            emitConstants(outfP, nodeP->value.symP);
            break;

        case TEXT:
            fprintf(outfP, "/ Text table\n");
            emitText(outfP, nodeP->value.flexText);
            fprintf(outfP, "/ End\n");
            noNl = true;
            break;

        case ASCII:
            fprintf(outfP, "/ Ascii table\n");
            emitAscii(outfP, nodeP->value.strP);
            fprintf(outfP, "/ End\n");
            noNl = true;
            break;

        case TYPE340:
            fprintf(outfP, "/ Type 340 character table\n");
            emitText(outfP, nodeP->value.flexText);
            fprintf(outfP, "/ End\n");
            noNl = true;
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
            noNl = true;
            break;

        case IMPORT:
            if( *(nodeP->value.strP) != '<' )
            {
                fprintf(outfP, "/ import \"%s\"\n", nodeP->value.strP);
            }
            else
            {
                fprintf(outfP, "/ import %s\n", nodeP->value.strP);
            }
            break;

        case EXPORT:
            fprintf(outfP, "/ export ");
            for( node2P = nodeP->rightP; node2P; node2P = node2P->leftP )
            {
                cP = (node2P->type == NAME)?node2P->value.strP:node2P->value.symP->name;
                fprintf(outfP, "%s%s", cP, (node2P->leftP)?", ":"\n");
            }
            break;

        case SEMI:
            fprintf(outfP, "\n");
            break;

        case EMPTYLINE:
            noNl = false;
            break;

        case TERMINATOR:
            if( !noNl )
            {
                fprintf(outfP, "\n");
            }

            noNl = false;
            sawTerm = true;
            break;

        default:
            // just ignore
            noNl = true;
            break;
        }

        if( (nodeP->type != TERMINATOR) && (nodeP->type != COMMENT) )
        {
            sawTerm = false;
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
        else if( nodeP->value.ival == LSHIFT )
        {
            // there is no << in macro1, have to reduce everything
            lval = onesComplAdj(evalExpr(nodeP->leftP));
            rval = onesComplAdj(evalExpr(nodeP->rightP));
            fprintf(outfP,"%o", (lval << rval) & WRDMASK);
        }
        else if( nodeP->value.ival == RSHIFT )
        {
            // there is no >> in macro1, have to reduce everything
            lval = onesComplAdj(evalExpr(nodeP->leftP));
            rval = onesComplAdj(evalExpr(nodeP->rightP));
            fprintf(outfP,"%o", (lval >> rval) & WRDMASK);
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
        if( nodeP->rightP )
        {
            fprintf(outfP,"\t/ %s", nodeP->rightP->value.strP);
        }
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
        if( doWarn(WARN_BREF) )
        {
            fprintf(stderr,"am1: WARNING: bank references are not supported by macro1!\n");
        }

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
    case PRIVATE:
    case ADDLOCAL:
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
emitText(FILE *outfP, FlexText flexText)
{
int i;
int val;
char *bufP;

    bufP = flexText.bufP;

    for( val = i = 0; i < flexText.nchars; i++ )
    {
        if( i && !(i % 3) )
        {
            fprintf(outfP, "    %06o\n", val);
            val = 0;
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
        fprintf(outfP, "    %06o\n", val);
    }
    else if( (i >= flexText.nchars) && !( i % 3) )
    {
        fprintf(outfP, "    %06o\n", val);      // didn't emit the word yet
    }
}

// Walk a list of variables, emit the storage
static void
emitVars(FILE *fP, PNodeListP listP)
{
PNodeP nodeP;
SymNodeP symP;

    while( listP )
    {
        nodeP = listP->nodeP;
        symP = nodeP->value.symP;
        fprintf(fP,"%s, %06o\n", symP->name, (nodeP->leftP)?evalExpr(nodeP->leftP):0);

        listP = listP->nextP;
    }
}

// Walk a symbol table of constants, emit the values
static int
emitConstants(FILE *fP, SymNodeP symP)
{
int val;

    if( !symP )
    {
        return(0);
    }

    fprintf(fP,"    %06o\n", symP->value2 & 0777777);

    val = emitConstants(fP, symP->leftP);
    val += emitConstants(fP, symP->rightP);

    return(val);
}
