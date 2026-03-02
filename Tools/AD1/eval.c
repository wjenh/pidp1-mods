// Various functions for handling values
// Also loads a symbol table.

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>

#include "ad1.h"
#include "y.tab.h"

#define NUMSYMBOLS 1024
#define SYMHEADER "%%am1 symtab file%%"

static Symbol symbols[NUMSYMBOLS];
static int symCount;           // number of symbols

extern int curBank;

SymbolP findSymbolByName(int bank, char *nameP);
int findAddrByName(int bank, char *nameP);
char *findNameByAddr(u32 addr);
bool loadSymbols(char *fnameP);
int parseString(char *strP, char** parts, char *breaksP);
int getNumber(char *stringP, int base);
int getValue(char *stringP, int base);
int signExtend(int oc);
int onesCompl(int val);
int twosCompl(int val);

extern bool isMemMapped(void);
extern int getLineFromAddress(int addr);

// See if this is a valid number, if so, return its value.
// The base is overridden by explicit base settings in the string, 0x, 0d, 0b, 0o.
// A leading 0 forces base 8.
// If not a number, return BADNUM.
int
getNumber(char *stringP, int base)
{
int newBase;
int value;
char *cP;

    newBase = base;

    // See if we override
    if( isdigit(*stringP) )
    {
        if( *stringP == '0' )
        {
            newBase = 8;        // maybe
        }

        if( *(stringP + 1) && (cP = strchr("dxb", *(stringP + 1))) )
        {
            switch( *cP )
            {
            case 'd':
                newBase = 10;
                stringP += 2;            // this is special to us, strtol doesn't understand it
                break;
            case 'x':
                newBase = 16;
                break;
            case 'b':
                newBase = 2;
                break;
                break;
            }
        }

        value = strtol(stringP, &cP, newBase);
        if( cP && *cP )
        {
            printf("'%s' is not a valid number.\n", stringP);
            return(BADNUM);
        }

        return( value );
    }

    return( BADNUM );
}

// If not a valid number string, try to look up a value from the symbol list.
// A symbol string can contain a bank qualifier: sym,num.
// The base is overridden by explicit base settings in the string, 0x, 0d, 0b.
// A leading 0 forces base 8.
// If not a number and no symbol found, return BADNUM.
int
getValue(char *stringP, int base)
{
int value;

    // Number?
    if( isdigit(*stringP) )
    {
        if( (value = getNumber(stringP, base)) == BADNUM )
        {
            printf("'%s' is not a valid number.\n", stringP);
            return(BADNUM);
        }
    }
    else if( (value = findAddrByName(curBank, stringP)) < 0 )
    {
        printf("There is no symbol '%s' found.\n", stringP);
        return(BADNUM);
    }

    return( value );
}

// Look for a symbol by name, return address if found else NIL
// The same symbol can be in different banks, so check for the correct one.
// We distinguish by bank or if the name has bank qualifier, it.
SymbolP
findSymbolByName(int bank, char *nameP)
{
int i;
char *cP, *cP2;
SymbolP symP;
char tmpstr[256];       // in case we have a bank qualifier

    if( (cP = strchr(nameP,',')) )
    {
        strncpy(tmpstr, nameP, cP - nameP);
        tmpstr[cP - nameP] = '\0';

        i = strtol(cP+1, NIL, 10);
        if( (i < 1) || (i > MAXBANKS) )
        {
            printf("Qualified symbol '%s' must have a decimal bank mumber of 1-%d\n", nameP, MAXBANKS);
            return(NIL);
        }

        nameP = tmpstr;
        bank = i;               // this overrides the passed bank
    }

    for( i = 0; i < symCount; ++i )
    {
        symP = &symbols[i];

        if( !strcmp(nameP, symP->nameP) && (bank == ((symP->address >> 12) & 0xF)) )
        {
            return( &symbols[i] );
        }
    }

    return(NIL);
}

// Look for a symbol by name, return the symtab entry pointer if so, else -1
// Note that the same symbol can be defined multiple times.
// We distinguish by bank or if the name has bank qualifier, it.
int
findAddrByName(int bank, char *nameP)
{
SymbolP symP;

    if( (symP = findSymbolByName(bank, nameP)) != NIL )
    {
        return( (int)symP->address );
    }
    else
    {
        return(-1);
    }
}

// Look for a symbol by address, return name if foune, else null
char *
findNameByAddr(u32 addr)
{
int i;

    for( i = 0; i < symCount; ++i )
    {
        if( symbols[i].address == addr )
        {
            return( symbols[i].nameP );
        }
    }

    return(NIL);
}

bool
loadSymbols(char *fnameP)
{
int lineno;
u32 addr;
char *cP, *cP2;
SymbolP symP;
FILE *fP;
char line[256];

    if( !(fP = fopen(fnameP,"r")) )
    {
        printf("Can't open symbol file '%s', no symbols will be available.\n", fnameP);
        return( false );
    }

    if( !fgets(line, sizeof(line), fP) )
    {
        printf("File '%s' is empty, no symbols will be available.\n", fnameP);
        fclose(fP);
        return( false );
    }

    line[strlen(line) - 1] = NUL;   // drop newline
    if( strcmp(line, SYMHEADER) )
    {
        printf("File '%s' not a valid symbol file, no symbols will be available.\n", fnameP);
        fclose(fP);
        return( false );
    }

    fgets(line, sizeof(line), fP);  // discard version line
    fgets(line, sizeof(line), fP);  // get file name

    printf("Loading symbols for program %s", line);

    while( fgets(line, sizeof(line), fP) )
    {
        if( line[0] == '#' )
        {
            continue;       // not currently used, but might be a comment eventually
        }

        line[strlen(line) - 1] = NUL;   // drop newline

        addr = strtol(line, &cP, 8);    // symbol addrs are always octal
        if( (*cP++ != ' ') || !strchr("GIX", *cP++) || (*cP++ != ' ') )
        {
            printf("File '%s' is not a valid symbol file, no symbols will be available.\n", fnameP);
            fclose(fP);
            symCount = 0;
            return( false );
        }

        // cP now points to the symbol name
        if( symCount >= NUMSYMBOLS )
        {
            // We'll keep what we have
            printf("Too many symbols have been seen, can't load more than %d\n.", NUMSYMBOLS);
            fclose(fP);
            return(false);
        }

        // Terminate the symbol name
        cP2 = cP;
        while( !isspace(*cP2) )
        {
            ++cP2;
        }

        *cP2 = NUL;

        symP = &symbols[symCount++];
        symP->address = addr;
        symP->nameP = malloc(strlen(cP) + 1);
        strcpy(symP->nameP, cP);

        // Now try for the line number.
        // If a .lst file has been loaded, look up the line from the memory map.
        // If not, use the current line number.
        if( isMemMapped() )
        {
            lineno = getLineFromAddress(addr);
        }

        if( lineno < 1 )
        {
            lineno = strtol(cP2 + 1, NIL, 10);
        }

        symP->lineno = lineno;
    }

    fclose(fP);
    return(true);
}

// Get rid of any cureent symbol defs.
void
clearSymbols()
{
int i;
SymbolP symP;

    for( i = 0; i < symCount; ++i )
    {
        symP = &symbols[i];
        if( symP->nameP )
        {
            free( symP->nameP );
            symP->nameP = NIL;
        }
    }

    symCount = 0;
}

// Given a number in 2s cmpl, convert to 1s cmpl.
// Only affects negative numbers.
int
onesCompl(int oc)
{
unsigned int i;

    i = (unsigned int)oc;

    if( oc < 0 )
    {
        i--;
        if( ((signed int)i == -1) )
        {
            i = 0;
        }

        oc = (signed int)i;
    }

    return(oc);
}

// Sign extend an 18 bit 1's complement number into a full int, still 1's complement.
int
signExtend(int oc)
{
    if( (oc & 0x20000) && !(oc & 0x40000) )
    {
        // it's negative
        // sign extend it
        oc |= -1 & ~0x3FFFF;
    }

    return( oc );
}

// Given a number in 18 bit 1s cmpl, convert to 2s cmpl.
// Only affects negative numbers.
int
twosCompl(int oc)
{
unsigned int i;

    if( oc < 0 )
    {
        i = (unsigned int)oc;
        i++;
        oc = (signed int)i;
    }

    return(oc);
}

// Evaluate an operation.
// The values will be 1's complement, handle doing math ops in 2's complement.
// The result will be 1's complement.
int
eval(int op, int lval, int rval)
{
int rslt;

    switch( op )
    {
    case OR:
        rslt = lval | rval;
        break;
    case XOR:
        rslt = lval ^ rval;
        break;
    case AND:
        rslt = lval & rval;
        break;
    case LSHIFT:
        rslt = lval << rval;
        break;
    case RSHIFT:
        rslt = lval >> rval;
        break;
    case CMPL:
        rslt = ~lval;
        break;

    default:
        lval = twosCompl(signExtend(lval));
        rval = twosCompl(signExtend(rval));
        switch( op )
        {
        case PLUS:
            rslt = lval + rval;
            break;
        case MINUS:
            rslt = lval - rval;
            break;
        case UMINUS:
            rslt = -lval;
            break;
        case MUL:
            rslt = lval * rval;
            break;
        case DIV:
            rslt = lval / rval;
            break;
        default:
            printf("Internal error, bad operator %d\n", op);
            break;
        }

        rslt = onesCompl(rslt);
    }
    
    return( rslt );
}
