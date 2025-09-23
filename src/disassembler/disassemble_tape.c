/*
 * Disassemble a punched tape that has RIM and/or BIN format binary load images.
 *
 * See comments on this source below.
 *
 * Tapes are loaded initially using read-in, which ignores any character that doesn't have bit 0200 set.
 * However, many tapes have a leader punched that shows a punch pattern that makes descriptive text.
 * This is also captured by printing out in 'label' form any character up to the first 0200 one.
 *
 * When tapes were loaded for real, the BIN loader would effectively stop at the end of a load because of a JMP
 * being executed. But, tapes sometimes had additional data following starting in RIM form again.
 * So, while there is data left in the tape image, reading and disassembly will continue after the BIN JMP, going
 ' back to RIM mode.
 * Note that some tapes seem to have random data instead; perhaps the remainder was read under program control.
 * Any data that isn't in a load block will be output as an octal number.
 * An EOT in the middle of a 3 char binary word set will be reported as an error on stderr,
 * A single 0377 at the 1st position in a word set followed by the end of the tape will be treated as a
 * STOP code and not be reported as an error.
 *
 * This is a two-pass disassembler to allow the generation of labels and validation of loaded memory addresses.
 * Labels are all of the form 'lnnn', e.g. l123.
 * Only memory addresses seen inside a RIM or BIN block are candidates for labels.
 * Anything outside of those blocks is not considered to be valid memory locations.
 *
 * All values are 18 bit, 3 tape chars.
 * Any char that doesn't have bit 0200 set is ignored to simulate the RPB instruction.
 * For the examples below, 0200 has been implicitly removed.
 *
 * RIM format
 *
 * 032 aaaa dddddd  store dddddd at adr aaaa, 6 tape chars
 * 060 aaaa         end and jmp to adr aaaa, 3 tape chars
 *
 * BIN format
 *
 * DIO startaddr, 32ssss
 * DIO endaddr + 1, 32eeee
 * data
 * ...
 * checksum
 * JMP aaaa, 60aaaa
 *
 * BIN loader that is typically loaded by an initial RIM: (digital-1-3-s-mb_ddt.bin)
 *
 * 7751: 730002		rpb          read tape
 * 7752: 327760		dio 7760     will be a 'dio startaddr' or a 'jmp progstart', deposit to 7760
 * 7753: 107760		xct 7760     execute what we just read; if a jmp, we're done otherwise it's meaningless
 * 7754: 327776		dio 7776     initialize checksum
 * 7755: 730002		rpb          read tape
 * 7756: 327777		dio 7777     deposit to 7777, will be 'dio endaddr + 1'
 * 7757: 730002		rpb          read tape, top of loading loop
 * 7760: 60aaaa		dio cur_addr put word in current pc location
 * 7761: 217760		lac i 7760   add the word we stored to the checksum
 * 7762: 407776		add 7776     add to checksum
 * 7763: 247776		dac 7776     update checksum
 * 7764: 447760		idx 7760     7760++, makes the dio point to the next adress to store in
 * 7765: 527777		sas 7777     skip if AC == 'dio endaddr + 1'
 * 7766: 607757		jmp 7757     not done, loop
 * 7767: 207776		lac 7776     add 'dio endaddr + 1' to checksum
 * 7770: 407777		add 7777     the computed checksum is is now in the AC
 * 7771: 730002		rpb          read tape, is checksum from tape
 * 7772: 327776		dio 7776     deposit to 7776
 * 7773: 527776		sas 7776     skip if AC == 7776
 * 7774: 760400		hlt          bad checksum
 * 7775: 607751		jmp 7751     ready for another block or a jmp, back to top
 * 7776: checksum
 * 7777: 32aaaa     dio endaddr + 1
 *
 * Comments:
 * This software may be freely used for any purpose as long as the author credit is kept.
 * It is strongly asked that the revision history be updated and any changes sent back to pdp1@quackers.net so
 * the master source can be maintained.
 *
 * A note on formatting:
 * This code uses the One Really True formatting style.
 * While it might appear verbose, as in braces around single if() bodies and braces on separate lines,
 * please follow it.
 * It is based upon some research into causes of errors in C done at Stanford many decades ago, refined
 * by use over 30+ years in a commercial environment both for C and Java.
 * And, yes, real programmers comment their code!
 *
 * Original author: Bill Ezell (wje), pdp1@quackers.net
 *
 * Revision history:
 *
 * 22/09/2025 wje - Initial version
 * 23/09/2025 wje - Convert to two pass
 *
 */
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>

#define DIAGNOSTIC(args...) if( diagnostics ) {printf(args); printf("\n");}

// Instructions have a 5 bit opcode followed by a 1 bit indirect marker as the high 6 bits of a word
#define OPERATION(x)    (x >> 12)

// The remaining 12 low bits are the operand whose meaning varies by instruction
#define OPERAND(x)      (x & 0007777)

// THe operate instruction is a pain, has to be done bitwise in ad-hoc code
#define OPR_MASK_CLA 00200
#define OPR_MASK_CLF 00007
#define OPR_MASK_CLI 04000
#define OPR_MASK_CMA 01000
#define OPR_MASK_HLT 00400
#define OPR_MASK_LAT 02000
#define OPR_MASK_NOP 07777
#define OPR_MASK_STF 00010

// The processing loop is a simple state machine
typedef enum {START, RESTART, LOOKING, RIM, BIN} State;

// Indicates instruction-specific additional processing needed
typedef enum {NONE, CAN_INDIRECT, IS_SKIP, IS_SHIFT, IS_OPR, IS_IOT, IS_LAW, IS_CALJDA, IS_ILLEGAL} Modifiers;

// IOT instructions have a number of special behaviors
typedef enum {UNKNOWN, NORMAL, CAN_WAIT, INVERT_WAIT, IS_DPY, IS_SZF, IS_SZS, HAS_ID} SpecialMods;

// defines one instruction
typedef struct
{
    char *name;
    Modifiers modifiers;
} CodeDef;

// This is used to handle instructions that aren't standard, such as the IOT, OPR, etc. instructions
typedef struct
{
    int value;              // the value of the operand field to match
    char *name;             // instruction name to output
    SpecialMods modifiers;  // eg, if the instruction can wait for I/O completion
    int mask;               // the bits in the operand to use for comparison to value, value == (oprerand & mask)
} Special;

int adjustOverflow(int);
int getWord(FILE *, int, int);
void formatInstr(int, int);
void printTapeLeader(int);
void passOne(FILE *);

void markUsed(int);
void markUsedByInstruction(int, int);
void markTarget(int);
int getLabel(int, char*);

void usage(void);
Special *findSpecial(Special *, int);

// Set from cmd line args
bool as_macro = false;
bool unknown_iots = false;
bool diagnostics = false;
bool keep_rim = false;

int pass = 1;               // first pass
int label_number = 1;       // used with memlocs and labels
int tape_loc = 0;
int checksum = 0;
State state;

char labelStr[16];          // for formatting a label

// This array is used for tracking labels.
// The low 9 bits are the label number.
// It is indexed by a memoery address.
short memlocs[4096];        // enough for the entire address space
#define MEM_VALID   01000   // this location was seen as a load address in RIM or BIN
#define MEM_TARGET  02000   // this location was seen as the target of a memoery reference

// Opcodes using only the high 5 bits, not the indirect bit, which means we only need 32 opcode entries.
// This generally works, except for the JDA instruction, 17, which is handled specially.

CodeDef opcodes[] =                 // we don't use the indirect bit, so we have only 32 possibilities
    {
        { 0, IS_ILLEGAL },                             // OP 0
        { "and", CAN_INDIRECT},                        // OP 2
        { "ior", CAN_INDIRECT},                        // OP 4
        { "xor", CAN_INDIRECT},                        // OP 6
        { "xct", CAN_INDIRECT},                        // OP 10
        { 0, IS_ILLEGAL},
        { 0, IS_ILLEGAL},
        { "cal", IS_CALJDA},                           // OP 16, special, could be JDA
        { "lac", CAN_INDIRECT},                        // OP 20
        { "lio", CAN_INDIRECT},                        // OP 22
        { "dac", CAN_INDIRECT},                        // OP 24
        { "dap", CAN_INDIRECT},                        // OP 26
        { "dip", CAN_INDIRECT},                        // OP 30
        { "dio", CAN_INDIRECT},                        // OP 32
        { "dzm", CAN_INDIRECT},                        // OP 34
        { 0, IS_ILLEGAL},
        { "add", CAN_INDIRECT},                        // OP 40
        { "sub", CAN_INDIRECT},                        // OP 42
        { "idx", CAN_INDIRECT},                        // OP 44
        { "isp", CAN_INDIRECT},                        // OP 46
        { "sad", CAN_INDIRECT},                        // OP 50
        { "sas", CAN_INDIRECT},                        // OP 52
        { "mul", CAN_INDIRECT},                        // OP 54
        { "div", CAN_INDIRECT},                        // OP 56
        { "jmp", CAN_INDIRECT},                        // OP 60
        { "jsp", CAN_INDIRECT},                        // OP 62
        { "skp", IS_SKIP},                             // OP 64
        { "sft", IS_SHIFT},                            // OP 66
        { "law", IS_LAW},                              // OP 70
        { "iot", IS_IOT},                               // OP 72
        { 0, IS_ILLEGAL},
        { "opr", IS_OPR}                               // OP 76
    };

// IOT decoding.
// The more specific masks should come first
Special iots[] =
    {
        {04074, "eem", NORMAL, 07777},
        {00074, "lem", NORMAL, 07777},
        {00001, "rpa", INVERT_WAIT, 0777},
        {00002, "rpb", INVERT_WAIT, 0777},
        {00033, "cks", NORMAL, 077},
        {00007, "dpy", IS_DPY, 077},
        {00055, "esm", NORMAL, 077},
        {00054, "lsm", NORMAL, 077},
        {00005, "ppa", INVERT_WAIT, 077},
        {00006, "ppb", INVERT_WAIT, 077},
        {00030, "rrb", NORMAL, 077},
        {00004, "tyi", INVERT_WAIT, 077},
        {00003, "tyo", INVERT_WAIT, 077},
        {00051, "asc", CAN_WAIT, 077},
        {00000, "iot", UNKNOWN, 0}       // special end marker if nothing mathces, must be last
    };

// Skip decoding.
Special skips[] =
    {
        {00400, "sma", NORMAL, 07777},
        {00200, "spa", NORMAL, 07777},
        {02000, "spi", NORMAL, 07777},
        {00100, "sza", NORMAL, 07777},
        {01000, "szo", NORMAL, 07777},
        {00000, "szf", IS_SZF, 07770},
        {01000, "szs", IS_SZS, 07707},
        {00000, "skp", UNKNOWN, 0}      // special end mareker if nothing matches
    };

// Shift/rotate decoding.
Special shifts[] =
    {
        {001000, "ral", NORMAL, 017000},
        {011000, "rar", NORMAL, 017000},
        {003000, "rcl", NORMAL, 017000},
        {013000, "rcr", NORMAL, 017000},
        {002000, "ril", NORMAL, 017000},
        {012000, "rir", NORMAL, 017000},
        {005000, "sal", NORMAL, 017000},
        {015000, "sar", NORMAL, 017000},
        {007000, "scl", NORMAL, 017000},
        {017000, "scr", NORMAL, 017000},
        {006000, "sil", NORMAL, 017000},
        {016000, "sir", NORMAL, 017000},
        {00000, "sft", UNKNOWN, 0}      // special end mareker if nothing matches, must be last
    };

int
main(int argc, char **argv)
{
int word, word2;
int cur_addr;
int end_addr;               // for BIN loader
int start_addr = 4;        // for macro start, can come from RIM if no BIN blocks, 4 is the default if none
char *filenameP;

bool did_start = false;

char *cP;

FILE *fP;
char tmpstr[16];

    // parse our comd line args
    while( --argc > 0 )
    {
        cP = *(++argv);

        if( *cP != '-' )
        {
            break;
        }

        while( *(++cP) )
        {
            switch( *cP )
            {
            case 'i':
                unknown_iots = true;
                break;

            case 'm':
                as_macro = true;
                break;

            case 'd':
                diagnostics = true;
                break;

            case 'k':
                keep_rim = true;
                break;

            default:
                usage();
                break;
            }
        }
    }

    if( argc != 1 )
    {
        usage();                // should only be the input filename
    }

    filenameP = cP;

    if( !(fP = fopen(filenameP, "r")) )
    {
        fprintf(stderr,"Can't open file '%s'\n", filenameP);
        exit(1);
    }

    passOne(fP);                // first pass finds all locations that need labels and validates the tape.
    fclose(fP);
    fP = fopen(filenameP, "r"); // reopen for second pass

    if( as_macro )
    {
        printf("Created by disasemble_tape -m %s\n", cP);
    }
    else
    {
        keep_rim = true;
        printf("Disassembly of file %s\n", cP);
    }

    // We don't check for errors because those were already detected by pass one
    state = START;
    DIAGNOSTIC("Pass two started");

    // The state machine loop
    for(;;)
    {
        word = getWord(fP, 2, state);

        if( word == -1 )
        {
            break;          // all done
        }

        switch( state )
        {
        case START:
        case RESTART:                               // only difference is that we don't print leader info
            if( OPERATION(word) == 032 )            // beginning of the RIM code block
            {
                state = RIM;
                cur_addr = OPERAND(word);

                if( as_macro )
                {
                    if( keep_rim )
                    {
                        printf("%o/\n", cur_addr);
                    }
                }
                else
                {
                    printf("Start of RIM block at tape position %d\n", tape_loc - 3);
                    printf("Addr  Raw    Lbl  Instruction\n");
                }

                word = getWord(fP, 2, state);
                if( keep_rim )
                {
                    formatInstr(cur_addr, word);
                }
            }
            break;

        case RIM:
            if( OPERATION(word) == 060 )        // end of RIM code block
            {
                state = LOOKING;                // look for a BIN block now
                start_addr = OPERAND(word);

                // Reset our address to 0, initial condition
                start_addr = cur_addr = 0;
            }
            else if( OPERATION(word) == 032 )   // next data word to load
            {
                cur_addr = OPERAND(word);
                word = getWord(fP, 2, state);

                if( keep_rim )
                {
                    formatInstr(cur_addr, word);
                }
            }
            break;

        case LOOKING:
            if( OPERATION(word) == 032 )   // RIM ended, DIO, beginning of BIN block
            {
                cur_addr = OPERAND(word);   // starting address

                if( !as_macro )
                {
                    printf("\nStarting BIN block at tape position %d\n", tape_loc - 3);
                    printf("Addr  Raw    Lbl  Instruction\n");
                }

                checksum = word;            // initial checksum

                if( as_macro )
                {
                    printf("%o/\n", cur_addr);  // set addr for macro
                }

                word = getWord(fP, 2, state);         // will be 'dio endaddr + 1'
                end_addr = OPERAND(word);       // last location + 1, in checksum as the dio instruction
                checksum += word;

                state = BIN;
            }
            else if( OPERATION(word) == 060 )   // JMP, done loading BIN blocks
            {
                if( as_macro )
                {
                    printf("      start %o\n", OPERAND(word)); // macro directive to give start addr
                    did_start = true;
                }

                state = RESTART;                // could be more on the tape, back to searching for RIM
            }
            else
            {
                // Not in a block, dump whatever it is with address 0
                formatInstr(0, word);
            }
            break;

        case BIN:
            {
                // word will contail the 18 bit value for the current pc
                formatInstr(cur_addr++, word);

                if( cur_addr >= end_addr )      // done, get the checksum from the tape, compare
                {
                    if( !as_macro )
                    {
                        printf("End of BIN block at tape position %d\n", tape_loc - 3);
                    }

                    word = getWord(fP, 2, state);         // checksum already checked in pass one
                    state = LOOKING;
                }
            }
            break;

        default:
            printf("Bad state %d\n", state);
            break;
        }
    }

    if( as_macro )
    {
        if( !did_start )                // we need to tell macro the starting addr, came from the RIM block
        {
            printf("     start %o\n", start_addr); // macro directive to give start addr
        }
    }
    else
    {
        printf("Done\n");
    }

    fclose( fP );
    exit(0);
}

void
passOne(FILE *fP)
{
int word;
int cur_addr;
int end_addr;               // for BIN loader
int start_addr = 4;        // for macro start, can come from RIM if no BIN blocks, 4 is the default if none

    state = START;
    DIAGNOSTIC("Starting pass one");
    DIAGNOSTIC("State is START");

    // The state machine loop
    for(;;)
    {
        word = getWord(fP, 1, state);

        if( word == -1 )
        {
            if( state == BIN )
            {
                printf("EOF inside BIN block near tape position %d!\n", tape_loc);
                fclose(fP);
                exit(1);
            }

            break;          // all done
        }

        switch( state )
        {
        case START:
        case RESTART:                               // only difference is that we don't print leader info
            if( OPERATION(word) == 032 )            // beginning of the RIM code block
            {
                state = RIM;
                DIAGNOSTIC("New state is RIM");
                cur_addr = OPERAND(word);

                if( as_macro )
                {
                    DIAGNOSTIC("RIM start");
                    if( keep_rim )
                    {
                        printf("%o/\n", cur_addr);
                    }
                }

                word = getWord(fP, 1, state);
                if( keep_rim )
                {
                    markUsedByInstruction(cur_addr, word);
                }
            }
            else
            {
                if( (state == START) && !as_macro )
                {
                    printf("Binary word %06o at tape position %d before RIM block, ignored.\n",
                        word, tape_loc);
                }
            }
            break;

        case RIM:
            if( OPERATION(word) == 060 )        // end of RIM code block
            {
                state = LOOKING;                // look for a BIN block now
                DIAGNOSTIC("New state is LOOKING");
                start_addr = OPERAND(word);

                if( as_macro )
                {
                    DIAGNOSTIC("RIM end, start addr is %04o", start_addr);
                }

                // Reset our address to 0, initial condition
                start_addr = cur_addr = 0;
            }
            else if( OPERATION(word) == 032 )   // next data word to load
            {
                cur_addr = OPERAND(word);
                word = getWord(fP, 1, state);
                markUsed(cur_addr);
            }
            else
            {
                fprintf(stderr,"Unterminated RIM block at tape position %d\n", tape_loc);
                fclose(fP);
                exit(1);
            }
            break;

        case LOOKING:
            if( OPERATION(word) == 032 )   // RIM ended, DIO, beginning of BIN block
            {
                cur_addr = OPERAND(word);   // starting address
                DIAGNOSTIC("BIN start, addr %04o", cur_addr);

                checksum = word;            // initial checksum
                word = getWord(fP, 1, state);         // will be 'dio endaddr + 1'

                if( OPERATION(word) != 032 )        // should have been another DIO
                {
                    fprintf(stderr,"Malformed BIN start, no 2nd DIO  at tape position %d\n", tape_loc - 3);
                    fclose(fP);
                    exit(1);
                }

                checksum += word;
                end_addr = OPERAND(word);
                state = BIN;
                DIAGNOSTIC("New state is BIN");
            }
            else if( OPERATION(word) == 060 )   // JMP, done loading BIN blocks
            {
                markTarget(OPERAND(word));
                state = RESTART;                // could be more on the tape, back to searching for RIM
                DIAGNOSTIC("New state is RESTART");
            }
            break;

        case BIN:
            {
                // word will contain the 18 bit value for the current pc, add to checksum
                checksum += word;
                markUsedByInstruction(cur_addr++, word);

                if( cur_addr >= end_addr )      // done, get the checksum from the tape, compare
                {
                    DIAGNOSTIC("End of BIN block");

                    checksum = adjustOverflow(checksum);
                    word = getWord(fP, 1, state);

                    if( as_macro )
                    {
                        if( word != checksum )
                        {
                            DIAGNOSTIC("Bad checksum at tape location %d", tape_loc - 3);
                        }
                    }

                    state = LOOKING;
                    DIAGNOSTIC("New state is LOOKING");
                }
            }
            break;

        default:
            printf("Bad state %d\n", state);
            break;
        }
    }

    markUsed(start_addr);
}

// Adjust a 32bit checksum to adjust for overflow, original was calculated using 1's complement 18 bit addition
// Taken from the macro1 cross-assembler.
int
adjustOverflow(int a)
{
    if( a & ~0777777 )
    {
        a = (a & 0777777) + (a >> 18);
    }

    if( a & 01000000 )			/* one more time */
    {
        a++;
    }

    return(a);
}


// Return the next 18 bit word, 3 tape characters, from the 'tape', or -1 if EOF.
// Since this is the equivalent of the RPB instruction, ignore any without bit o200 set.
// But, if pass 2 and seen before a binary-flagged byte, print the byte as a label part, suppressing extra blanks.

int
getWord(FILE *fP, int pass, int state)
{
int word;
int count;
int ch;
int last_ch;
int saw_space;

    word = 0;
    count = 3;              // we need 3 tape bytes for a word
    saw_space = 0;
    ch = last_ch = 0;       // character read before the current character, needed for STOP detection

    while( count )
    {
        if( (ch = fgetc(fP)) == EOF )
        {
            if( (count == 2) && (last_ch == 0377) )
            {
                return( -1 );                  // Stop marker? 
            }

            if( count != 3 )
            {
                fprintf(stderr,"Premature EOF at tape position %d\n", tape_loc);
                fclose(fP);
                exit(1);
            }

            return( -1 );           // sorry
        }

        ++tape_loc;

        if( ch & 0200 )
        {
            last_ch = ch;       // will be the previous char
            word = (word << 6) | (ch & 077);
            --count;
        } 
        else if( (pass == 2) && (state == START) )   // still in leader
        {
            if( ch == 0 )           // suppress multiple spaces
            {
                saw_space = 1;
            }
            else
            {
                if( saw_space )
                {
                    printf("\n");
                    saw_space = 0;
                }

                if( !as_macro )
                {
                    printTapeLeader(ch);
                }
            }
        }
    }
    
    return( word );
}

Special *
findSpecial(Special *specP, int op)
{
int i;

    for( ;; )
    {
        if( specP->value == (op & specP->mask) )   // found it
        {
            break;
        }
        else if( specP->modifiers == UNKNOWN )
        {
            break;                          // should be end of list
        }

        specP++;
    }

    return( specP );
}

void
printTapeLeader(int ch)
{
int i;
char tmpstr[16];

    // Assume it's a leader label as punched by macro
    for( i = 0; i < 8; i++ )
    {
        tmpstr[i] = (ch & 0200)?'*':' ';
        ch <<= 1;
    }
    
    tmpstr[8] = 0;
    printf("%s\n",tmpstr);
}

// format an instruction into printed form
void
formatInstr(int pc, int word)
{
int opcode;
int indirect;
int operand;
int tmp, tmp2;
CodeDef *instructionP;
Special *sP;

    if( as_macro )
    {
        if( getLabel(pc, labelStr) )
        {
            printf("%s,\n", labelStr);
        }

        printf("     ");           // line indent for macro
    }
    else
    {
        printf("%04o: %06o %4s", pc, word, (getLabel(pc, labelStr))?labelStr:"    ");
    }

    opcode = OPERATION(word);
    indirect = opcode & 01;
    opcode >>= 1;
    operand = OPERAND(word);
    instructionP = &opcodes[opcode];

    if( instructionP->modifiers == IS_ILLEGAL )            // not an instruction, leave blank unless macro mode
    {
        if( as_macro )
        {
            printf(" %06o\n", word);
        }
        else
        {
            printf("\n");
        }

        return;
    }

    switch( instructionP->modifiers )
    {
    case NONE:
        printf(" %s", instructionP->name);
        break;

    case CAN_INDIRECT:
        getLabel(operand, labelStr);
        printf(" %s%s %4s", instructionP->name, (indirect)?" i":"", labelStr);
        break;

    case IS_CALJDA:
        if( indirect )
        {
            getLabel(operand, labelStr);
            printf(" jda %4s", labelStr);
        }
        else
        {
            printf(" %s", instructionP->name);            // CAL
        }
        break;

    case IS_LAW:
        printf(" %s %s%04o", instructionP->name, (indirect)?"-":"", operand);
        if( !as_macro )
        {
            printf(" (%d dec)", (indirect)?-operand:operand);
        }
        break;

    case IS_SHIFT:
        tmp + ((indirect?010000:0) | operand) & 017000;      // indirect bit controls left-right
        sP = findSpecial(shifts, tmp);
        if( sP->modifiers == UNKNOWN )
        {
            printf(" %06o", word);                           // a bare shift doesn't exist, must be data
        }
        else
        {
            // Shift/rotate is strange. The number of places to shift/rotate is the count of 1's in the lower 9 bits
            tmp = operand & 0777;
            tmp2 = 0;
            while( tmp != 0 )
            {
                if( tmp & 01 )
                {
                    ++tmp2;
                }
                
                tmp >>= 1;
            }

            printf(" %s  %3ds", sP->name, tmp2);
        }
        break;

    case IS_SKIP:
        sP = findSpecial(skips, operand);
        printf(" %s", sP->name);
        if( indirect )
        {
            printf(" %s", as_macro?"i":"not");
        }

        switch( sP->modifiers )
        {
        case UNKNOWN:
            printf(" %04o", operand);
            break;

        case IS_SZF:
            printf(" %0o", (operand & 07));
            break;

        case IS_SZS:
            printf(" %0o", ((operand >> 3) & 07));
            break;
        }
        break;

    case IS_OPR:
        // This one is a pain because multiple operations can be combined.
        tmp = 0;                // set to 1 if we have printed one already

        printf(" ");

        if( (operand & OPR_MASK_CLA) && !(operand & OPR_MASK_LAT) )
        {
            printf("%scla", tmp?"!":"");
            tmp = 1;
        }
        if( operand & OPR_MASK_CLI )
        {
            printf("%scli", tmp?"!":"");
            tmp = 1;
        }
        if( operand & OPR_MASK_CMA )
        {
            printf("%scma", tmp?"!":"");
            tmp = 1;
        }
        if( operand & OPR_MASK_HLT )
        {
            printf("%shlt", tmp?"!":"");
            tmp = 1;
        }
        if( (operand & OPR_MASK_CLA) && (operand & OPR_MASK_LAT) )
        {
            printf("%slat", tmp?"!":"");
            tmp = 1;
        }
        if( (operand & OPR_MASK_NOP) == 0 )
        {
            printf("%sopr", tmp?"!":"");        // maacro seems to use this for an operate that does nothing
            tmp = 1;
        }
        if( operand & OPR_MASK_STF )
        {
            printf("%sstf %o", tmp?"!":"", operand & 07);
            tmp = 1;
        }
        if( (operand & OPR_MASK_CLF) && !(operand & OPR_MASK_STF) )
        {
            printf("%sclf %o", tmp?"!":"", operand & 07);
            tmp = 1;
        }
        break;

    case IS_IOT:
        sP = findSpecial(iots, operand);
        printf(" %s", sP->name);

        switch( sP->modifiers )
        {
        case UNKNOWN:
            if( operand != 0 )                  // not an empty IOT
            {
                if( indirect )
                {
                    operand |= 010000;       // include the bit in the output
                }

                printf("%s%05o", (as_macro)?"!":" ", operand);
                if( unknown_iots )
                {
                    fprintf(stderr, "iot %05o\n", operand);
                }
            }
            break;

        case CAN_WAIT:
            if( indirect )
            {
                printf(" %s", as_macro?"i":"w");
            }
            break;

        case INVERT_WAIT:       // because macro automatically sets, xx-i means not set
            if( !indirect )
            {
                printf("-i");
            }
            break;

        case IS_DPY:            // does INVERT_WAIT plus intensity
            if( !indirect )
            {
                printf("-i");
            }

            tmp = operand & 0700;  // macro doesn't print the intensity if it's zero 
            if( tmp > 0 )
            {
                printf(" %3o", tmp);
            }
            break;

        case HAS_ID:            // some encode a subdevice, eg tape drive number
            printf(" %02o", (operand >> 6) & 077);
            break;
        }
        break;
    }

    printf("\n");
}

// Set memory location as used, i.e. was loaded by loader
void
markUsed(int address)
{
    memlocs[address] |= MEM_VALID;
    DIAGNOSTIC("%06o marked as used", address);
}

// Mark the address as used, and if word is an instruction that references memory, mark the target also
void
markUsedByInstruction(int addr, int word)
{
int opcode;
int operand;
CodeDef *instructionP;

    markUsed(addr);

    opcode = OPERATION(word);
    opcode >>= 1;               // ignore indirect bit
    operand = OPERAND(word);
    instructionP = &opcodes[opcode];
    if( instructionP->modifiers == CAN_INDIRECT )
    {
        markTarget( operand );
    }
}

// Set memory location as used and the target of an instruction, e.g. JMP address
void
markTarget(int address)
{
    if( !(memlocs[address] & MEM_TARGET) )
    {
        DIAGNOSTIC("%06o marked as target L%d", address, label_number);
        memlocs[address] |= (MEM_TARGET | label_number++);
    }
}

// Return the label number if the address has been loaded and is the target of a memory reference, else 0
// Format the proper result, either a label or the address if no label defined.
int
getLabel(int address, char* labelP)
{
    if( (memlocs[address] & (MEM_VALID | MEM_TARGET)) == (MEM_VALID | MEM_TARGET) )
    {
        address = memlocs[address] & 0777;       // is now the label numbe
        sprintf(labelP,"L%03d", address);
        return( address );
    }
    else
    {
        sprintf(labelP,"%04o",address);          // no label assigned
        return( 0 );
    }
}

void usage()
{
    fprintf(stderr,"Usage: disassemble_tape [-midk] filename\n");
    fprintf(stderr,"where:\n");
    fprintf(stderr,"m - output in pure macro assember form\n");
    fprintf(stderr,"i - print any unknown IOTs on stderr\n");
    fprintf(stderr,"d - enable diagnostics for debugging this progam\n");
    fprintf(stderr,"k - keep RIM loader code if seen and in macro mode; normally no because MACRO usually adds it\n");
    fprintf(stderr,"Flags can be together, -mid, or separate, -m -i -d\n");
    exit(1);
}
