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

#define NUMSYMBOLS 4096
#define SYMHEADER "%%am1 symtab file%%"

static Symbol symbols[NUMSYMBOLS];
static int symCount;           // number of symbols

extern int base;               // current numeric output base
extern int curBank;
extern int curFileNo;

SymbolP findSymbolByName(int bank, int fileNo, char *nameP);
int findAddrByName(int bank, int fileNo, char *nameP);
char *findNameByAddr(u32 addr);
bool loadSymbols(FileInfoP infoP);
void listSymbols(void);
int parseString(char *strP, char** parts, char *breaksP);
int getNumber(char *stringP, int base);
int getValue(char *stringP, int base);
int signExtend(int oc);
int onesCompl(int val);
int twosCompl(int val);
int adjustZero(int val);

extern char *getFormat(int fmtType);

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
    else if( (value = findAddrByName(curBank, curFileNo, stringP)) < 0 )
    {
        printf("There is no symbol '%s' found in file %d.\n", stringP, curFileNo);
        return(BADNUM);
    }

    return( value );
}

// Look for a symbol by name, return address if found else NIL
// The same symbol can be in different banks, so check for the correct one.
// We distinguish by bank and fileNo.
// However, if fileNo is NOARG, don't check the file number.
SymbolP
findSymbolByName(int bank, int fileNo, char *nameP)
{
int i;
SymbolP symP;

    for( i = 0; i < symCount; ++i )
    {
        symP = &symbols[i];

        if( !strcmp(nameP, symP->nameP) && (bank == BANKOF(symP->address)) &&
            ((fileNo == NOARG) || (symP->fileNo == fileNo)) )
        {
            return( symP );
        }
    }

    return(NIL);
}

// Walk the symbol list, print out the values.
void
listSymbols()
{
int i;
SymbolP symP;

    printf("Address File Name\n");
    for( i = 0; i < symCount; ++i )
    {
        symP = &symbols[i];
        printf(getFormat(OCTAL), symP->address);
        printf("  %d    %s\n", symP->fileNo + 1, symP->nameP);
    }
}

// See if a symbol is in the current file.
// Return it if so, else NIL.
SymbolP
findSymbolInCurrentFile(char *nameP)
{
int i;
SymbolP symP;

    for( i = 0; i < symCount; ++i )
    {
        symP = &symbols[i];

        if( !strcmp(nameP, symP->nameP) && (symP->fileNo == curFileNo) )
        {
            return( symP );
        }
    }

    return(NIL);
}

// See if a symbol in the current file has the given address.
// Return it if so, else NIL.
SymbolP
findAddressInCurrentFile(int address)
{
int i;
SymbolP symP;

    for( i = 0; i < symCount; ++i )
    {
        symP = &symbols[i];

        if( (symP->address == address)  && (symP->fileNo == curFileNo) )
        {
            return( symP );
        }
    }

    return(NIL);
}

// Look for a symbol by name, return the symtab entry pointer if so, else -1
// Note that the same symbol can be defined multiple times.
// We distinguish by bank or if the name has bank qualifier, it, as well as file number.
// If fileNo is NOARG, only look in the current file.
// If there is a symbol that is defined in the current file, use it.
// If a fileNo is given and not found there but it is defined in another file,
// use that one and make the current file that file.
// If found, return the address, else return -1.
int
findAddrByName(int bank, int fileNo, char *nameP)
{
SymbolP symP;

    if( (fileNo == NOARG) && (symP = findSymbolInCurrentFile(nameP)) )
    {
        return(symP->address);
    }

    if( (symP = findSymbolByName(bank, fileNo, nameP)) != NIL )
    {
        curFileNo = symP->fileNo;
        return( (int)symP->address );
    }
    else
    {
        return(-1);
    }
}

// Look for a symbol by address, return name if found, else nil.
char *
findNameByAddr(u32 addr)
{
int i;
SymbolP symP;

    if( (symP = findAddressInCurrentFile(addr)) )
    {
        return(symP->nameP);
    }

    for( i = 0; i < symCount; ++i )
    {
        symP = &symbols[i];

        if( symP->address == addr )
        {
            curFileNo = symP->fileNo;
            return( symP->nameP );
        }
    }

    return(NIL);
}

bool
loadSymbols(FileInfoP infoP)
{
u32 addr;
char *cP, *cP2;
SymbolP symP;
MapEntryP entryP;
FILE *fP;
char line[256];

    if( !infoP->symNameP || !*(infoP->symNameP) )
    {
        return( false );
    }

    if( !(fP = fopen(infoP->symNameP,"r")) )
    {
        printf("Can't open symbol file '%s', no symbols will be available.\n", infoP->symNameP);
        return( false );
    }

    if( !fgets(line, sizeof(line), fP) )
    {
        printf("File '%s' is empty, no symbols will be available.\n", infoP->symNameP);
        fclose(fP);
        return( false );
    }

    line[strlen(line) - 1] = NUL;   // drop newline
    if( strcmp(line, SYMHEADER) )
    {
        printf("File '%s' not a valid symbol file, no symbols will be available.\n", infoP->symNameP);
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
            printf("File '%s' is not a valid symbol file, no symbols will be available.\n", infoP->symNameP);
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
        symP->fileNo = infoP->fileNo;
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

// Given a 1's complement value that is an 18 bit -0, convert to 0.
// The result will still be 1's complement.
int
adjustZero(int val)
{
    if( (val & 0777777) == 0777777 )
    {
        val = 0;
    }

    return(val);
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
        oc = (signed int)i;
    }

    return(oc);
}

// Sign extend an 18 bit 1's complement number into a full int, still 1's complement.
int
signExtend(int oc)
{
    // We actually have a full C int, check bit 17 using rational bit positions, this would be bit 0 in -1 speak.
    // If bit 18, one bit higher than the most significant bit in an 18 bit word, is zero, then
    // the number is not sign extended, so extend it.
    if( (oc & 0x20000) && !(oc & 0x40000) )
    {
        // it's negative
        // sign extend it
        oc |= -1 & ~0x3FFFF;
    }

    return( oc );
}

// Given a number in 1s cmpl, convert to 2s cmpl.
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
// Note that all calculations are done using a full C int; the value will be truncated
// to 18 bits if it is stored to the -1 in the set command.
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
        rslt = lval << twosCompl(rval);
        break;
    case RSHIFT:
        rslt = lval >> twosCompl(rval);
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
        case MUL:
            rslt = lval * rval;
            break;
        case DIV:
            rslt = lval / rval;
            break;
        case MOD:
            rslt = lval % rval;
            break;

        case UMINUS:
            // Easier to deal with this in 1's complement
            lval = onesCompl(lval);         // switch it back
            rslt = adjustZero(~lval);       // and it's still 1's complement
            return( rslt );

        default:
            printf("Internal error, bad operator %d\n", op);
            break;
        }

        rslt = adjustZero(onesCompl(rslt));
    }
    
    return( rslt );
}
