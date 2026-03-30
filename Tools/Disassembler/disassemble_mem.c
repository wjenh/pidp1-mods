/*
 * Disassemble a section of memory represented by a memory image file created by pidp-1.
 *
 *
 * This program disassembles memory data from a memory file into macro assembly instructions.
 * Two modes are supported.
 *
 * The default mode is a verbose disassembly with detailed information about the disassembled content, but it cannot
 * be assembled.
 *
 * The second mode is am1 mode, which will generate bank information since am1 supports extended memory.
 *
 * The third mode is macro1 mode, which will generate output that can be assembled by macro1 if extended
 * memory is not being used.
 * It will generate labels for referenced locations.
 * It cannot be assembled by the native PDP-1 assembler.
 *
 * The memory range is limited to the size of one memory bank, 4096 words.
 *
 * This is a two-pass disassembler to allow the generation of labels and validation of loaded memory addresses.
 * Labels are all of the form 'Lnnn', e.g. L123.
 *
 * All values are 18 bits and shown in octal.
 *
 * Comments:
 * This software may be freely used for any purpose as long as the author credit is kept.
 * It is strongly asked that the revision history be updated and any changes sent back to pdp1@quackers.net so
 * the master source can be maintained.
 *
 * This uses the decodeInstr() method form the decode_instruction.c file.
 *
 * Please maintain the formatting.
 *
 * Original author: Bill Ezell (wje), pdp1@quackers.net
 *
 * Revision history:
 *
 * 29-Mar-2026 wje - Initial version
 *
 */
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <libgen.h>

#define MEMFILE "/opt/pidp1-mods/coremem"

#define MEMSIZE 4096    // words in a memory bank
#define BANKS 16        // and the number of banks we support
#define BANKOF(a) ((a) & ~(MEMSIZE - 1))   // get the bank part of a full 16 bit address, 4 bits
#define BANKNUM(a) (BANKOF(a) >> 12)       // get the bank number of a full 16 bit address

// Used for label tracking
#define MEM_VALID  01       // this location was seen as a load address in RIM or BIN
#define MEM_TARGET 02       // this location was seen as the target of a memoery reference

#define DIAGNOSTIC(args...) if( diagnostics ) {printf(args); printf("\n");}

// Instructions have a 5 bit opcode followed by a 1 bit indirect marker as the high 6 bits of a word
// IOTs can also have a completion-requested, bit 6 set, 04000.
#define OPERATION(x)    (((x) >> 13) & 037)
// The remaining 12 low bits are the operand whose meaning varies by instruction
#define OPERAND(x)      (x & 0007777)

// This is used to hold label information
typedef struct
{
    short flags;
    char label[6];
} Label;

// Set from cmd line args
bool diagnostics = false;
bool asMacro = false;
bool asAm1 = false;

int curBank;                // used for am1 mode
int lastAddr;
int pass = 1;               // first pass
int label_number = 1;       // used with memlocs and labels
char separator[4];          // used in macro mode
char labelStr[16];          // for formatting a label

// This array is used for tracking labels.
// The low 9 bits are the label number.
// It is indexed by a memory address.
Label memlocs[MEMSIZE];       // enough for the entire address space
int memory[MEMSIZE];          // the memory image that was loaded

int getNumber(char *strP);
int getWord(FILE *, int, int);
bool readMem(FILE *fileP, int bank);

int getLabel(int, int,  char*);
void formatInstr(FILE *fP, int, int);
void passOne(int startAddr, int endAddr);
void markValid(int);
void markValidByInstruction(int, int);
void markTarget(int);

void usage(void);
void fail(void);
void validateAddress(int addr, int limit);

extern char *decodeInstr(int word, int addr, bool asMacro, char *separatorP, char *symbolP, char *resultP);
extern bool opCanIndirect(int opcode);

int
main(int argc, char **argv)
{
int opt;
int bank = 0;              // 0-15
int cur_addr = 0;
int end_addr = 07750;      // the full bank minus the standard loader
int start_addr = -1;

bool did_start = false;
char *cP;
char *filenameP;
FILE *infP;
FILE *outfP;

char filename[256];
char shortname[256];
char tmpstr[16];

    outfP = stdout;

    while( (opt = getopt(argc, argv, "b:a:e:s:o:m1d")) != -1 )
    {
        switch( opt )
        {
        case 'b':
            if( (bank = getNumber(optarg)) < 0 )
            {
                fail();
            }
            break;

        case 's':
            if( (cur_addr = getNumber(optarg)) < 0 )
            {
                fail();
            }

            validateAddress(cur_addr, MEMSIZE);
            break;

        case 'e':
            if( (end_addr = getNumber(optarg)) < 0 )
            {
                fail();
            }

            validateAddress(end_addr, MEMSIZE);
            break;

        case 'a':
            // The start address can be a full 16 bit address
            if( (start_addr = getNumber(optarg)) < 0 )
            {
                fail();
            }

            validateAddress(start_addr, BANKS*MEMSIZE);
            break;

        case 'o':
            if( !(outfP = fopen(optarg, "w")) )
            {
                fprintf(stderr,"Can't open input file '%s'\n", filenameP);
                exit(1);
            }
            break;

        case '1':
            asAm1 = true;
            strcpy(separator, " ");
            break;

        case 'm':
            asMacro = true;
            strcpy(separator, "!");         // is a logical or in macro1
            break;

        case 'd':
            diagnostics = true;
            break;

        default:
            usage();
            break;
        }
    }

    if( start_addr < 0 )
    {
        start_addr = cur_addr;
    }

    if( optind < argc )
    {
        filenameP = argv[optind];
        ++optind;
    }
    else
    {
        filenameP = MEMFILE;
    }

    if( optind < argc )
    {
        usage();                // should only be the input filename
    }

    if( !(infP = fopen(filenameP, "r")) )
    {
        fprintf(stderr,"Can't open input file '%s'\n", filenameP);
        exit(1);
    }

    if( !readMem(infP, bank) )
    {
        fprintf(stderr,"Memory file doesn't contain any data for bank %d\n", bank);
        exit(1);
    }

    fclose(infP);

    DIAGNOSTIC("Pass one started");
    passOne(cur_addr, end_addr);                // first pass finds all locations that need labels

    if( asMacro )
    {
        strcpy(shortname, basename(filenameP));

        // sorry, can't have a dot.
        if( (cP = strrchr(shortname, '.')) )
        {
            *cP = '\0';
        }

        fprintf(outfP, "Disassembled from %s\n", shortname);
        fprintf(outfP, "ioh=iot i\n");   // just makes things easier
    }
    else
    {
        fprintf(outfP, "Disassembled from %s\n", filenameP);
    }

    fprintf(outfP, "%o/\n", cur_addr);
    DIAGNOSTIC("Pass two started");

    curBank = 0;

    for( ;cur_addr <= end_addr; ++cur_addr )
    {
        formatInstr(outfP, cur_addr, memory[cur_addr]);
    }

    if( asMacro || asAm1 )
    {
        fprintf(outfP, "     start %o\n", start_addr); // directive to give start addr
    }
    else
    {
        fprintf(outfP,"Done.\n");
    }

    fclose( outfP );
    exit(0);
}

void
passOne(int cur_addr, int end_addr)
{
char tmpstr[16];

    for( ;cur_addr <= end_addr; ++cur_addr )
    {
        markValidByInstruction(cur_addr, memory[cur_addr]);
    }
}

// format an instruction into printed form
void
formatInstr(FILE *outfP, int pc, int word)
{
int tmp;
bool needBank = false;
char symbolstr[256];
char tmpstr[256];

    if( BANKNUM(pc) != curBank )
    {
        if( asMacro )
        {
            fprintf(outfP, "/ WARNING - this code uses extended memory, it will not work properly!\n");
        }

        needBank = true;
        curBank = BANKNUM(pc);
    }

    if( getLabel(pc, pc, labelStr) != -1 )
    {
        if( asMacro || asAm1 )
        {
            strcat(labelStr,",");
        }

        for( tmp = strlen(labelStr) + 1; tmp++ < 6; )
        {
            strcat(labelStr, " ");
        }
    }
    else
    {
        labelStr[0] = '\0';
    }

    if( !asMacro && !asAm1 )
    {
        if( needBank )
        {
            fprintf(outfP, (asMacro)?"/ Now in bank %o\n":"bank %o\n", curBank);
        }

        if( pc != (lastAddr + 1) )
        {
            fprintf(outfP, "%o/\n", pc & 07777);    // addr is relative to current bank
        }

        lastAddr = pc;
    }
    else
    {
        if( needBank )
        {
            fprintf(outfP, "%06o: bank %o\n",  pc, curBank);
        }
    }

    if( asMacro || asAm1 )
    {
        fprintf(outfP, "%s", (labelStr[0] != '\0')?labelStr:"     ");
    }
    else
    {
        fprintf(outfP, "%04o: %06o %s", pc, word, (labelStr[0] != '\0')?labelStr:"     ");
    }

    getLabel(word & 07777, word & 07777, symbolstr);
    decodeInstr(word, word & 07777, asMacro || asAm1, separator, symbolstr, tmpstr);

    fprintf(outfP, "%s\n", tmpstr);
}

// Set memory location as used, i.e. was loaded by loader
void
markValid(int address)
{
    address = OPERAND(address);         // for safety, limits it to 12 bits

    memlocs[address].flags |= MEM_VALID;
    DIAGNOSTIC("%06o marked as valid", address);
}

// Mark the address as used, and if word is an instruction that references memory, mark the target also
void
markValidByInstruction(int addr, int word)
{
int opcode;
int operand;

    markValid(addr);            //this address is used

    opcode = OPERATION(word);
    operand = OPERAND(word);
    DIAGNOSTIC("opcode %o, operand %o", opcode, operand);
    if( opCanIndirect(opcode) )
    {
        markTarget( operand );
    }
}

// Set memory location as used and the target of an instruction, e.g. JMP address
void
markTarget(int address)
{
int itmp, itmp2;
char ch;
char *cP;

    if( !(memlocs[address].flags & MEM_TARGET) )
    {
        DIAGNOSTIC("%06o marked as target number %d", address, label_number);
        memlocs[address].flags |= MEM_TARGET;

        // construct the label
        cP = memlocs[address].label;

        sprintf(cP, "L%d", label_number);
        ++label_number;
        DIAGNOSTIC("Constructed label is %s\n", memlocs[address].label);
    }
}

// Return the label number if the address has been loaded and is the target of a memory reference, else -1
// Format the proper result, either a label or the address if no label defined.
int
getLabel(int address, int defaultval, char* labelP)
{
char *cP;

    address = OPERAND(address);                   // for safety

    if( (memlocs[address].flags & (MEM_VALID | MEM_TARGET)) == (MEM_VALID | MEM_TARGET) )
    {
        sprintf(labelP,"%s", memlocs[address].label);
        return( address );
    }
    else
    {
        sprintf(labelP,"%04o", defaultval);      // no label assigned
        return( -1 );
    }
}

// Read the memory file into our memory image.
// Return true if successful, false if EOF.
bool
readMem(FILE *fileP, int bank)
{
int addr;
int word;
char *strP;
bool didOne = false;

char buf[100];

    while( (strP = fgets(buf, 100, fileP)) )
    {
        if( *strP )
        {
            if( *strP == ';' )      // haven't seen this in the mem file, but it's in the original read code
            {
                break;
            }
            else if( (*strP >= '0') && (*strP <= '7') )
            {
                word = strtol(strP, &strP, 8);

                if( *strP == ':' )  // address line
                {
                    addr = word;
                    if( (addr >> 12) < bank )
                    {
                        continue;
                    }
                    else if( (addr >> 12) > bank )
                    {
                        // all done
                        return(didOne);
                    }
                }
                // add to memory
                memory[addr & 07777] = word;
                didOne = true;
            }
            else
            {
                fprintf(stderr, "Line '%s` is invalid.\n", buf);
                return(false);
            }
        }
    }

    return(didOne);
}

// Get a valid number in a strtol() compatible base.
// Return -1 if not valid
int
getNumber(char *strP)
{
int num;
char *cP;

    num = strtol(strP, &cP, 0);
    if( !*cP )
    {
        return(num);
    }
    else
    {
        return(-1);
    }
}

void
fail()
{
    usage();
    exit(1);
}

void
validateAddress(int addr, int limit)
{
    if( addr >= limit )
    {
        fprintf(stderr,"The address must be less than 0%o, %d decimal. Use bank to select the memory bank.\n",
            limit, limit);
        exit(1);
    }
}

void
usage()
{
    fprintf(stderr,"Usage: disassemble_mem [-m|1] [-b nn] [-s nnnn] [-e nnnn] [-a nnnn] [-o filename] [memfile]\n");
    fprintf(stderr,"where:\n");
    fprintf(stderr,"m - output in macro, else listing form\n");
    fprintf(stderr,"1 - (digit one) output in am1, else listing form\n");
    fprintf(stderr,"b - disassemble from this bank, 0-15 decimal, the default is 0\n");
    fprintf(stderr,"s - start from this address, the default is 0\n");
    fprintf(stderr,"e - end at this address, the default is 07750\n");
    fprintf(stderr,"a - set the start address to this address, the default is the disassemble start address\n");
    fprintf(stderr,"o - the output file, the default is stdout\n");
    fprintf(stderr,"memfile - the memory file, the default is /opt/pidp1-mods/coremem\n");
    fprintf(stderr,"Numbers can be given as 0nnnn, nnnn, 0xnnnn as recognized by strtol().\n");
    exit(1);
}
