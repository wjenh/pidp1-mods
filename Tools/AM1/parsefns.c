/* parsefns.c - support routines for parser */
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include "am1.h"
#include "y.tab.h"

PNodeP unop(int, int, PNodeP);
PNodeP binnop(int, int, PNodeP, PNodeP);
void yyerror(char *);

PNodeP
newnode(int pc, int type, PNodeP leftP, PNodeP rightP)
{
    PNodeP nP;
    if(!(nP = (PNodeP) calloc(1, sizeof(PNode))))
    {
        yyerror("out of memory in newnode()");
    }

    nP->pc = pc;
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
binop(int pc, int op, PNodeP lhsP, PNodeP rhsP)
{
PNodeP nodeP;

    nodeP = newnode(pc, BINOP, lhsP, rhsP);
    nodeP->value.ival = op;
    return(nodeP);
}

PNodeP
unop(int pc, int op, PNodeP rhsP)
{
PNodeP nodeP;

    nodeP = newnode(pc, UNOP, NILP, rhsP);
    nodeP->value.ival = op;
    return(nodeP);
}

// Ascii to Flex/Concise conversions

// Bit 0100 means unshifted
// Bit 0200 means shifted
// Neither means no case
static int ascii2concise[] = {
	NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, // 00
	0075, 0036, NONE, NONE, NONE, 0077, NONE, NONE,
	NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE,
	NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE,

	0000, 0205, 0201, 0204, NONE, NONE, 0206, 0202, // 40
	0157, 0155, 0273, 0254, 0133, 0154, 0173, 0121,
	0120, 0101, 0102, 0103, 0104, 0105, 0106, 0107,
	0110, 0111, NONE, NONE, 0207, 0233, 0210, 0221,

	0140, 0261, 0262, 0263, 0264, 0265, 0266, 0267, // 100
	0270, 0271, 0241, 0242, 0243, 0244, 0245, 0246,
	0247, 0250, 0251, 0222, 0223, 0224, 0225, 0226,
	0227, 0230, 0231, 0257, 0220, 0255, 0211, 0240,

	0156, 0161, 0162, 0163, 0164, 0165, 0166, 0167, // 140
	0170, 0171, 0141, 0142, 0143, 0144, 0145, 0146,
	0147, 0150, 0151, 0122, 0123, 0124, 0125, 0126,
	0127, 0130, 0131, NONE, 0256, NONE, 0203, NONE
};

// Returns NONE (-1) if the character should be ignored.
// If a shift is seen and we aren't in the correct shift state, become so and return the shift char.
// The caller should then repeat the call with the original character to get the properly shifted one.
char
asciiToFlexo(char ac, int *shiftP)
{
int fc;
    
    fc = ascii2concise[ac];
    if( fc == NONE )
    {
        return(NONE);
    }

    if( (fc & 0200) && !*shiftP )      // we have a shifted char, but we're not shifted
    {
        *shiftP = 1;
        return( CSHIFT );
    }

    if( (fc & 0100) && *shiftP )      // we have an unshifted char, but we're shifted
    {
        *shiftP = 0;
        return( CUNSHIFT );
    }

    return( fc & 077 );
}
