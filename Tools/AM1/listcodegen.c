/*
 * Process a parse tree to generate a listing.
*/
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "am1.h"
#include "y.tab.h"

extern bool sawBank;
extern BankContextP banksP;

extern int countText(char *strP);
extern int countAscii(char *strP);
extern int evalExpr(PNodeP);
extern int onesComplAdj(int);

static void startLine(FILE *outP, PNodeP nodeP);
static void listStatements(FILE *, PNodeP);
static void listOperand(FILE *, PNodeP);
static void listAscii(FILE *outfP, PNodeP nodeP, char *strP);
static void listText(FILE *outfP, PNodeP nodeP, char *strP);
static void listVars(FILE *outfP, PNodeP nodeP);
static void listConstants(FILE *outfP, PNodeP nodeP, SymNodeP symP);

void verror(char *msgP, ...);

// Walk a tree and list a listing
int
listCodegen(FILE *outfP, PNodeP rootP)
{
    // The root is a HEADER.
    // The root lhs is the program body, the rhs the START at the end of the program.
    fprintf(outfP,"%s\n", rootP->value.strP);
    listStatements(outfP, rootP->leftP);

    if( sawBank )       // finish trailing consts
    {
        for(BankContextP bankP = banksP; bankP; bankP = bankP->nextP)
        {
        PNode node;

            if( bankP->constSymP )
            {
                node.pc = bankP->cur_pc;    // need a node for the pc
                fprintf(outfP, "/ Constants for bank %d\n", bankP->bank);
                listConstants(outfP, &node, bankP->constSymP);
            }
        }
    }

    fprintf(outfP,"start %o\n", rootP->rightP->value.ival);
    return(1);
}

static void
listStatements(FILE *outfP, PNodeP nodeP)
{
PNodeP node2P;
char str[128];

    while( nodeP )
    {
        switch( nodeP->type )
        {
        case COMMENT:
            fprintf(outfP, "/ %s\n", nodeP->value.strP);
            break;

        case ORIGIN:
            startLine(outfP, nodeP);
            fprintf(outfP, "%o/", nodeP->value.ival);
            if( nodeP->rightP )
            {
                listOperand(outfP, nodeP->rightP);
            }
            break;

        case EXPR:
            startLine(outfP, nodeP);
            listOperand(outfP, nodeP->rightP);
            break;

        case LOCATION:
        case LCLLOCATION:
            startLine(outfP, nodeP);
            fprintf(outfP, "%s,", nodeP->value.symP->name);
            if( nodeP->rightP )
            {
                fprintf(outfP," ");
                listOperand(outfP, nodeP->rightP);
            }
            break;

        case VARS:
            fprintf(outfP,"/ variables\n");
            listVars(outfP, nodeP->rightP);
            break;

        case BANK:
            startLine(outfP, nodeP);
            fprintf(outfP,"bank %d", nodeP->value.ival );
            break;

        case CONSTANTS:
            startLine(outfP, nodeP);
            fprintf(outfP," constants\n");
            listConstants(outfP, nodeP, nodeP->value.symP);
            break;

        case TEXT:
            fprintf(outfP, "/ Text table\n");
            listText(outfP, nodeP, nodeP->value.strP);
            fprintf(outfP, "/ End\n");
            break;

        case ASCII:
            fprintf(outfP, "/ Ascii table\n");
            listAscii(outfP, nodeP, nodeP->value.strP);
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
listOperand(FILE *outfP, PNodeP nodeP)
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
        listOperand(outfP, nodeP->leftP);
        switch( nodeP->value.ival )
        {
        case XOR:
            ch = '^';
            break;
        case DIV:
            ch = '/';
            break;
        case MOD:
            ch = '%';
            break;
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
            verror("unknown binary op %d in listOperand",
                nodeP->value.ival);
        }

        fprintf(outfP,"%c", ch);
        listOperand(outfP, nodeP->rightP);
        break;

    case UNOP:
        switch( nodeP->value.ival )
        {
        case PARENS:
            fprintf(outfP,"(");
            listOperand(outfP, nodeP->rightP);
            fprintf(outfP,")");
            break;
        case UMINUS:
            fprintf(outfP,"-");
            listOperand(outfP, nodeP->rightP);
            break;
        case CMPL:
            fprintf(outfP, "~");
            listOperand(outfP, nodeP->rightP);
            break;
        default:
            verror("unknown unary op %d in listOperand", nodeP->value.ival);
        }
        break;

    case CONSTANT:
        fprintf(outfP,"[");
        listOperand(outfP, nodeP->value.symP->ptr);
        fprintf(outfP,"]");
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
        fprintf(outfP,"%s:%d", nodeP->value.symP->name, nodeP->value2.ival );
        break;

    case LCLADDR:
        symP = nodeP->value.symP;
        fprintf(outfP, "%s", symP->name);
        break;

    case ADDR:
        symP = nodeP->value.symP;
        fprintf(outfP, "%s", symP->name );
        break;

    case LITCHAR:
        fprintf(outfP, "'\\%03o'", nodeP->value.ival);
        break;

    case CHAR:
        fprintf(outfP, "char '%c'", nodeP->value.ival);
        break;

    case FLEXO:
        fprintf(outfP, "flexo %06o", nodeP->value.ival);
        break;

    case INTEGER:
        fprintf(outfP, "%o", nodeP->value.ival & WRDMASK);
        break;

    case LOCAL:
        fprintf(outfP, "local");
        break;

    case ENDLOC:
        fprintf(outfP, "endloc");
        break;

    case FORCELOC:
        fprintf(outfP, "%%forcelocal");
        break;

    case TERMINATOR:
        fprintf(outfP, "\n");
        break;

    default:
        verror("unknown op %d in listOperand", nodeP->type);
    }
}

// Emit packed ascii
static void
listAscii(FILE *outfP, PNodeP nodeP, char *strP)
{
int i;

    fprintf(outfP,"/ ascii \"%s\"\n", strP);
    for( i = 0; *strP != 0; i ^= 1 )
    {
        if( !i )
        {
            startLine(outfP, nodeP);
            nodeP->pc++;    // because we only get the initial node
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
        startLine(outfP, nodeP);
        fprintf(outfP, " 000000\n");
    }
}

// Emit packed flexo code
static void
listText(FILE *outfP, PNodeP nodeP, char *strP)
{
int i;
int val;

    fprintf(outfP,"/ flexo \"%s\"\n", strP);
    startLine(outfP, nodeP);
    for( val = i = 0; *strP != 0; i++ )
    {
        if( i && !(i % 3) )
        {
            fprintf(outfP, " %06o\n", val);
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
        startLine(outfP, nodeP);
        fprintf(outfP, " %06o\n", val);
    }
    else if( !*strP && !( i % 3) )
    {
        fprintf(outfP, " %06o\n", val);      // didn't list the word yet
    }
}

// Walk a list of variables, list the storage
static void
listVars(FILE *fP, PNodeP nodeP)
{
SymNodeP symP;

    while( nodeP )
    {
        symP = nodeP->value.symP;
        startLine(fP, nodeP);
        fprintf(fP,"%s, ", symP->name);
        (nodeP->leftP)?listOperand(fP,nodeP->leftP):0;
        fprintf(fP,"\n");
        nodeP = nodeP->rightP;
    }
}

// Walk a symbol table of constants, list the values
static void
listConstants(FILE *fP, PNodeP nodeP, SymNodeP symP)
{
    if( !symP )
    {
        return;
    }

    startLine(fP, nodeP);
    nodeP->pc++;    // because we only get the initial node
    fprintf(fP," %06o\n", symP->value2);

    listConstants(fP, nodeP, symP->leftP);
    listConstants(fP, nodeP, symP->rightP);
}

static void
startLine(FILE *outP, PNodeP nodeP)
{
    fprintf(outP, "%06o %06o ", nodeP->pc, nodeP->value2.ival);
}
