/* parsefns.c - support routines for parser */
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include "am1.h"
#include "y.tab.h"

PNodeP unop(int, int, int, PNodeP);
PNodeP binop(int, int, int, PNodeP, PNodeP);
void checkPCBound(char *mstP, int pc, int lineNo);

extern void yyerror(char *);
extern void verror(const char *msgP, ...);
extern void verrorl(int lineNo, const char *msgP, ...);

extern int curBank;
extern int curOverlay;

void
checkPCBound(char *msgP, int addr, int lineNo)
{
    if( addr >= BANKSIZE )                 // ran over the end of the memory
    {
        // Report against the line the caller recorded, not the global lineno.
        // If bison read the terminator as lookahead before the rule reduced,
        // the global has already moved on to the next line.
        verrorl(lineNo, "%s would go past the memory bank size of 4096 words", msgP);
    }
}

PNodeP
newnode(int lineNo, int pc, int type, PNodeP leftP, PNodeP rightP)
{
PNodeP nP;

    if(!(nP = (PNodeP) calloc(1, sizeof(PNode))))
    {
        yyerror("out of memory in newnode()");
    }

    nP->lineNo = lineNo - 1;       // it will already have been incremented by the line terminator in flex
    nP->pc = pc;
    nP->bank = curBank;
    nP->overlay = curOverlay;
    nP->originP = stmtOriginP;
    nP->type = type;
    nP->leftP = leftP;
    nP->rightP = rightP;
    return(nP);
}

void
freenodes(PNodeP nodeP)
{
    PNodeP n2P;
    while(nodeP)
    {
        freenodes(nodeP->leftP);
        n2P = nodeP->rightP;
        free(nodeP);
        nodeP = n2P;
    }
}

PNodeP
binop(int lineNo, int pc, int op, PNodeP lhsP, PNodeP rhsP)
{
PNodeP nodeP;

    nodeP = newnode(lineNo, pc, BINOP, lhsP, rhsP);
    nodeP->value.ival = op;
    return(nodeP);
}

PNodeP
unop(int lineNo, int pc, int op, PNodeP rhsP)
{
PNodeP nodeP;

    nodeP = newnode(lineNo, pc, UNOP, NILP, rhsP);
    nodeP->value.ival = op;
    return(nodeP);
}
