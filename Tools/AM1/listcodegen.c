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
extern int lineno;
extern BankContextP banksP;

extern int evalExpr(PNodeP);
extern char flexoToAscii(char fchar, int *shift);

static void startLine(bool noValue, FILE *outP, PNodeP nodeP);
static void listStatements(FILE *, PNodeP);
static void listOperand(FILE *, PNodeP);
static void listAscii(FILE *outfP, PNodeP nodeP, char *strP);
static void listText(FILE *outfP, PNodeP nodeP, FlexText text);
static bool listVar(FILE *outfP, PNodeP nodeP);
static void listVars(FILE *outfP, PNodeP nodeP);
static void listConstants(FILE *outfP, PNodeP nodeP, SymNodeP symP);
static void flxToA(FlexText flexText, char *rsltP);
static void formatCcomment(FILE *outfP, PNodeP nodeP);

void verror(char *msgP, ...);

// Walk a tree and list a listing
int
listCodegen(FILE *outfP, PNodeP rootP)
{
    // The root is a HEADER.
    // The root lhs is the program body, the rhs the START or STOP at the end of the program.
    fprintf(outfP,"%s\n", rootP->value.strP);
    listStatements(outfP, rootP->leftP);

    // Do any trailing constants and vars
    for(BankContextP bankP = banksP; bankP; bankP = bankP->nextP)
    {
    PNode node;

        if( bankP->constSymP )
        {
            node.pc = bankP->cur_pc;    // need a node for the pc
            node.bank = bankP->bank;
            node.lineNo = lineno;
            fprintf(outfP, "// Constants for bank %d\n", bankP->bank);
            listConstants(outfP, &node, bankP->constSymP);
        }

        if( bankP->varNodesP )
        {
            node.pc = bankP->cur_pc;    // need a node for the pc
            node.bank = bankP->bank;
            node.lineNo = lineno;
            node.value.ptr = bankP->varNodesP;
            fprintf(outfP, "// Variables for bank %d\n", bankP->bank);
            listVars(outfP, &node);
        }
    }

    if( rootP->rightP->type == START )
    {
        fprintf(outfP,"start %o\n", rootP->rightP->value.ival);
    }
    else
    {
        fprintf(outfP,"stop\n");
    }
    return(1);
}

static void
listStatements(FILE *outfP, PNodeP nodeP)
{
int i, j;
PNodeP node2P;
char *cP;
char str[128];

    while( nodeP )
    {
        switch( nodeP->type )
        {
        case COMMENT:
            if( nodeP->flags & PN_SOL )
            {
                // The comment was a line by itself
                fprintf(outfP, "%4d:               //%s\n", nodeP->lineNo, nodeP->value.strP);
            }
            else
            {
                fprintf(outfP, "      //%s\n", nodeP->value.strP);
            }
            break;

        case CSCOMMENT:
            formatCcomment(outfP, nodeP);
            break;

        case FILENAME:
            fprintf(outfP, "File %s\n", nodeP->value.strP);
            break;

        case ORIGIN:
            startLine(true, outfP, nodeP);
            fprintf(outfP, "%o/", nodeP->value.ival);
            if( nodeP->rightP )
            {
                listOperand(outfP, nodeP->rightP);
            }
            else
            {
                fprintf(outfP,"\n");
            }
            break;

        case EXPR:
            startLine(false, outfP, nodeP);
            listOperand(outfP, nodeP->rightP);
            break;

        case LOCATION:
        case LCLLOCATION:
            startLine((nodeP->rightP)?false:true, outfP, nodeP);
            fprintf(outfP, "%s,", nodeP->value.symP->name);
            if( nodeP->rightP )
            {
                fprintf(outfP," ");
                listOperand(outfP, nodeP->rightP);
            }
            break;

        case VARS:
            fprintf(outfP,"// variables\n");
            listVars(outfP, nodeP);
            break;

        case BANK:
            startLine(true, outfP, nodeP);
            fprintf(outfP,"bank %d", nodeP->value.ival );
            break;

        case CONSTANTS:
            startLine(true, outfP, nodeP);
            fprintf(outfP,"constants\n");
            listConstants(outfP, nodeP, nodeP->value.symP);
            break;

        case TEXT:
            fprintf(outfP, "// Text table\n");
            listText(outfP, nodeP, nodeP->value.flexText);
            fprintf(outfP, "// End\n");
            break;

        case TABLE:
            fprintf(outfP,"// table %o\n", nodeP->value.ival);
            if( nodeP->rightP )     // has initializer
            {
                j = evalExpr(nodeP->rightP);

                for( i = 0; i < nodeP->value.ival; ++i )
                {
                    startLine(false, outfP, nodeP);
                    nodeP->pc++;
                    fprintf(outfP, "    %o\n", j);
                }
            }
            else
            {
                startLine(false, outfP, nodeP);
                fprintf(outfP, "    . = .+%o\n", nodeP->value.ival);
            }

            fprintf(outfP, "// End");
            break;

        case ASCII:
            fprintf(outfP, "// Ascii table\n");
            listAscii(outfP, nodeP, nodeP->value.strP);
            fprintf(outfP, "// End");
            break;

        case SEMI:
            fprintf(outfP, ";\n");
            break;

        case TERMINATOR:
            fprintf(outfP, "\n");       // a bare terminator doesn't get a line number if output
            break;

        case VAR:
            startLine(true, outfP, nodeP);
            fprintf(outfP, "var ");
            listVar(outfP, nodeP->rightP);
            break;

        case IMPORT:
            startLine(true, outfP, nodeP);
            if( *(nodeP->value.strP) != '<' )
            {
                fprintf(outfP, "import \"%s\"", nodeP->value.strP);
            }
            else
            {
                fprintf(outfP, "import %s", nodeP->value.strP);
            }
            break;

        case EXPORT:
            startLine(true, outfP, nodeP);
            fprintf(outfP, "export ");
            for( node2P = nodeP->rightP; node2P; node2P = node2P->leftP )
            {
                cP = (node2P->type == NAME)?node2P->value.strP:node2P->value.symP->name;
                fprintf(outfP, "%s%s", cP, (node2P->leftP)?", ":"");
            }
            break;

        default:
            // just ignore
            break;
        }

        nodeP = nodeP->leftP;
    }
}

// Format and output an expression
static void
listOperand(FILE *outfP, PNodeP nodeP)
{
int lval;
int rval;
char *cP;
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
            cP = "^";
            break;
        case DIV:
            cP = "/";
            break;
        case MOD:
            cP = "%";
            break;
        case PLUS:
            cP = "+";
            break;
        case MINUS:
            cP = "-";
            break;
        case MUL:
            cP = "*";
            break;
        case AND:
            cP = "&";
            break;
        case OR:
            cP = "|";
            break;
        case LSHIFT:
            cP = "<<";
            break;
        case RSHIFT:
            cP = ">>";
            break;
        case SEPARATOR:
            cP = " ";
            break;
        default:
            verror("unknown binary op %d in listOperand",
                nodeP->value.ival);
        }

        fprintf(outfP,"%s", cP);
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
        if( nodeP->rightP )
        {
            // can only be a comment
            fprintf(outfP,"  // %s\n", nodeP->rightP->value.strP);
        }
        else
        {
            fprintf(outfP,"\n");
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

    case VALUESPEC:
        fprintf(outfP, "%s", nodeP->value.symP->name);
        break;

    case FORCELOC:
        fprintf(outfP, "%%forcelocal");
        break;

    case LOCAL:
    case ADDLOCAL:
        fprintf(outfP, (nodeP->type == LOCAL)?"local":"addlocal");
        if( (node2P = nodeP->rightP) )
        {
            while( node2P )
            {
                if( node2P->type == ADDR )
                {
                    cP = node2P->value.symP->name;      // this was a local override of a global
                }
                else
                {
                    cP = node2P->value.strP;
                }

                fprintf(outfP, " %s%s", cP, (node2P->leftP)?",":"");
                node2P = node2P->leftP;
            }
        } 
        break;

    case ENDLOC:
        fprintf(outfP, "endloc");
        if( nodeP->value.ival != -1 )
        {
            fprintf(outfP, " %d", nodeP->value.ival);
        }
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

    fprintf(outfP,"// ascii \"%s\"\n", strP);
    for( i = 0; *strP != 0; i ^= 1 )
    {
        if( !i )
        {
            startLine(false, outfP, nodeP);
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
        startLine(false, outfP, nodeP);
        fprintf(outfP, " 000000\n");
    }
}

// Emit packed flexo code
static void
listText(FILE *outfP, PNodeP nodeP, FlexText flexText)
{
int i;
int val;
char *bufP;
char buf[256];

    flxToA(flexText, buf);
    fprintf(outfP,"// text \"%s\"\n", buf);
    bufP = flexText.bufP;

    // Node will only have the pc of the first word, adjust as we go
    for( val = i = 0; i < flexText.nchars; i++ )
    {
        if( i && !(i % 3) )
        {
            startLine(false, outfP, nodeP);
            nodeP->pc++;
            fprintf(outfP, " %06o\n", val);
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

        startLine(false, outfP, nodeP);
        fprintf(outfP, " %06o\n", val);
    }
    else if( (i >= flexText.nchars) && !( i % 3) )
    {
        startLine(false, outfP, nodeP);
        fprintf(outfP, " %06o\n", val);      // didn't list the word yet
    }
}

// Variable, walk the list of names and list them.
// Each node's rightP is the next one, leftP is the optional initializer.
// However, the list is in reverse order, so handle that.
// Returns true if one was printed and a comman is needed.
static bool
listVar(FILE *fP, PNodeP nodeP)
{
int val;
bool needComma = false;
SymNodeP symP;

    if( !nodeP )
    {
        return(false);
    }

    needComma = listVar(fP, nodeP->rightP);

    symP = nodeP->value.symP;
    if( needComma )
    {
        fprintf(fP, ", ");
    }

    fprintf(fP, "%s", symP->name);
    if( nodeP->leftP )
    {
        fprintf(fP, "=");
        listOperand(fP,nodeP->leftP);
    }

    return(true);
}

// Variables, walk the variables, list the storage
static void
listVars(FILE *fP, PNodeP nodeP)
{
int val;
int lineNo;
PNodeListP listP;
SymNodeP symP;

    lineNo = nodeP->lineNo;
    listP = (PNodeListP)(nodeP->value.ptr);
    while( listP )
    {
        nodeP = listP->nodeP;
        nodeP->lineNo = lineNo;       // because these nodes were defined back in the var stmt
        symP = nodeP->value.symP;
        nodeP->value2.ival = (nodeP->leftP)?evalExpr(nodeP->leftP):0;
        startLine(false, fP, nodeP);
        fprintf(fP,"%s, ", symP->name);

        if( nodeP->leftP )
        {
            listOperand(fP,nodeP->leftP);
        }
        else
        {
            fprintf(fP,"0");
        }

        fprintf(fP,"\n");
        listP = listP->nextP;
    }
}

// Walk a symbol table of constants, list the values.
// If auto is true, this is being called to emit constants when there was no constants statement.
static void
listConstants(FILE *fP, PNodeP nodeP, SymNodeP symP)
{
    if( !symP )
    {
        return;
    }

    nodeP->value2.ival = symP->value2;  // set the value
    startLine(false, fP, nodeP);
    nodeP->pc++;    // because we only get the initial node
    fprintf(fP,"\n");

    listConstants(fP, nodeP, symP->leftP);
    listConstants(fP, nodeP, symP->rightP);
}

static void
startLine(bool noValue, FILE *outP, PNodeP nodeP)
{
    if( noValue )
    {
        // Line has no emitted code, don't print a value
        fprintf(outP, "%4d: %02o%04o        ", nodeP->lineNo, nodeP->bank, nodeP->pc);
    }
    else
    {
        fprintf(outP, "%4d: %02o%04o %06o ",  nodeP->lineNo, nodeP->bank, nodeP->pc, nodeP->value2.ival);
    }
}

// Convert a string containing flex chars into ascii
static void
flxToA(FlexText text, char *outP)
{
int i;
int shift = 0;
int ch;
char *cP;
    
    cP = text.bufP;

    for(i = 0; i < text.nchars; ++i )
    {
        ch = flexoToAscii(*cP++, &shift);
        if( ch == NONE )
        {
            continue;
        }

        *outP++ = ch;
    }

    *outP = 0;
}

// Deal with C-style comments which can span lines.
static void
formatCcomment(FILE *outfP, PNodeP nodeP)
{
int lineNo;
char *curP;
char *nextP;

    lineNo = nodeP->lineNo;
    curP = nextP = nodeP->value.strP;

    while( nextP )
    {
        ++lineNo;
        if( (nextP = strchr(curP, '\n')) )
        {
            *nextP++ = '\0';
        }

        fprintf(outfP, "%4d:           %s", lineNo, curP);
        curP = nextP;

        if( nextP )
        {
            fprintf(outfP,"\n");
        }
    }
    
    fprintf(outfP,"*/\n");
}
