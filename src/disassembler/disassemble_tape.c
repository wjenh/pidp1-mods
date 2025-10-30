/*
 * Disassemble a punched tape that has RIM and/or BIN format binary load images.
 *
 * See comments on this source below.
 *
 * This program disassembles PDP-1 binary tape images into macro assembly instructions.
 * Three modes are supported.
 * The default mode is a verbose disassembly with detailed information about the disassembled content, but it cannot
 * be assembled.
 *
 * The second mode is macro1 mode, which will generate output that can be assembled by the macro1 and variant
 * assemblers. It will generate labels for referenced locations.
 * It cannot be assembled by the native PDP-1 assembler.
 *
 * The third mode generates output suitable for the native assember, but lacks any label information.
 *
 * The PDP-1 binary tapes are loaded initially using read-in, which ignores any character that doesn't have
 * bit 0200 set.
 * However, many tapes have a leader punched that shows a punch pattern that makes descriptive text.
 * This is also captured by printing out in 'readable label' form any character up to the first 0200 one.
 *
 * Processing then continues looking for a RIM block, followed by ddt-form BIN blocks.
 * 
 * When tapes were loaded, the BIN loader would effectively stop at the end of a load because of a JMP
 * being executed. But, tapes sometimes had additional data following.
 * For macro modes, this must end processing because the emitted start statement must be the last thing
 * in the emitted sourde.
 * Any remaining data is ignored.
 *
 * In default mode, while there is data left in the tape image, reading and disassembly will continue.
 *
 * An EOT in the middle of a 3 char binary word set will be reported as a warning on stderr,
 * A single 0377 at the 1st position in a word set followed by the end of the tape will be treated as a
 * stp code and not be reported as an error.
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
 * DDT BIN format
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
 * 24/09/2025 wje - Make macro-style formatting of labels nicer, fixes for 'instructions' that are actually data
 * 24/09/2025 wje - Add raw mode for tapes that don't have a standard loader, just dump everything as instructions
 * 25/09/2025 wje - Various fixes around OPR and such.
 *                  IMPORTANT - assemblers, native and cross, are broken for law -n,
 *                  so if a binary for 'law -n' is seen generate 'safe' law i n which will give the correct result.
 *                  The behavior of law -n does not match the original DEC Macro documentation.
 *                  Instead of law -n generating effectively law i n, the actual negative number is added to the
 *                  law opcode, producing garbage.
 * 25/09/2025 wje - Add a -s switch to generate 3 char labels for backwards compatibility
 * 27/09/2025 wje - Continue to process a tape even if a malformed BIN block is found, jsut give a warning.
 * 28/09/2025 wje - Add -c for compalibility mode, will emit source that works with the native PDP-1 macro assembler.
 * 03/10/2025 wje - Handle non-standard tapes better, bail if in macro mode, dump in default mode.
 *
 */
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <libgen.h>

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
#define OPR_MASK_LAT 02200
#define OPR_MASK_NOP 07777
#define OPR_MASK_STF 00010

// The processing loop is a simple state machine
typedef enum {START, RESTART, LOOKING, RIM, BIN, DATA, RAW, DONE} State;

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

// This is used to hold label information
typedef struct
{
    short flags;
    char label[6];
} Label;

int addOnesComplement(int, int);
int getWord(FILE *, int, int);
void formatInstr(int, int);
void printTapeLeader(int);
void passOne(FILE *);
void pushbackWord(int);

void markValid(int);
void markValidByInstruction(int, int);
void markTarget(int);
int getLabel(int, int,  char*);

void usage(void);
Special *findSpecial(Special *, int);

// Set from cmd line args
bool as_macro = false;
bool raw_mode = false;
bool unknown_iots = false;
bool diagnostics = false;
bool keep_rim = false;
bool compatibility_mode = false;

int pass = 1;               // first pass
int label_number = 1;       // used with memlocs and labels
int tape_loc = 0;
int checksum = 0;
int saved_word = -1;        // for getWord() pushback
State state;
char separator[4];          // used in macro and compatibility modes

char labelStr[16];          // for formatting a label

// This array is used for tracking labels.
// The low 9 bits are the label number.
// It is indexed by a memoery address.
Label memlocs[4096];       // enough for the entire address space

#define MEM_VALID  01       // this location was seen as a load address in RIM or BIN
#define MEM_TARGET 02       // this location was seen as the target of a memoery reference

// Opcodes using only the high 5 bits, not the indirect bit, which means we only need 32 opcode entries.
// This generally works, except for the JDA instruction, 17, which is handled specially.

CodeDef opcodes[] =                 // we don't use the indirect bit, so we have only 32 possibilities
    {
        { "illegal", IS_ILLEGAL },                     // OP 0
        { "and", CAN_INDIRECT},                        // OP 2
        { "ior", CAN_INDIRECT},                        // OP 4
        { "xor", CAN_INDIRECT},                        // OP 6
        { "xct", CAN_INDIRECT},                        // OP 10
        { "illegal", IS_ILLEGAL},
        { "illegal", IS_ILLEGAL},
        { "cal", IS_CALJDA},                           // OP 16, special, could be JDA
        { "lac", CAN_INDIRECT},                        // OP 20
        { "lio", CAN_INDIRECT},                        // OP 22
        { "dac", CAN_INDIRECT},                        // OP 24
        { "dap", CAN_INDIRECT},                        // OP 26
        { "dip", CAN_INDIRECT},                        // OP 30
        { "dio", CAN_INDIRECT},                        // OP 32
        { "dzm", CAN_INDIRECT},                        // OP 34
        { "illegal", IS_ILLEGAL},
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
        { "iot", IS_IOT},                              // OP 72
        { "illegal", IS_ILLEGAL},
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
        {00004, "tyi", CAN_WAIT, 077},
        {00003, "tyo", CAN_WAIT, 077},
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
        {00000, "szs", IS_SZS, 07707},
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
char filename[256];
char shortname[256];

bool did_start = false;

char *cP;

FILE *fP;
char tmpstr[16];

    strcpy(separator, "!");         // is a logical or in macro1

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

            case 'r':
                raw_mode = true;
                break;

            case 'c':
                compatibility_mode = true;
                as_macro = true;
                strcpy(separator, " ");         // just used in some comments
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

    strcpy(filename,cP);

    if( !(fP = fopen(filename, "r")) )
    {
        fprintf(stderr,"Can't open file '%s'\n", filename);
        exit(1);
    }

    if( !as_macro )
    {
        keep_rim = true;            // in verbose mode, always emit the rim data
    }

    if( !raw_mode )
    {
        passOne(fP);                // first pass finds all locations that need labels and validates the tape.
        fclose(fP);
        fP = fopen(filename, "r"); // reopen for second pass
        tape_loc = 0;               // reset tape position
    }

    if( as_macro )
    {
        strcpy(shortname, basename(filename));

        // sorry, can't have a dot.
        if( (cP = strrchr(shortname, '.')) )
        {
            *cP = '\0';
        }

        printf("Disassembled from %s\n", shortname);
    }
    else
    {
        printf("Disassembled from %s\n", filename);
    }

    // We don't check for errors because those were already detected by pass one
    state = (raw_mode)?RAW:START;
    DIAGNOSTIC("Pass two started");

    // The state machine loop
    for(;;)
    {
        if( state == DONE )
        {
            break;
        }

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

                if( !as_macro )
                {
                    printf("Start of RIM block at tape position %d\n", tape_loc - 3);
                    printf("Tape  Addr  Raw    Lbl   Instruction\n");
                }

                word = getWord(fP, 2, state);
                if( keep_rim )
                {
                    if( as_macro )                  // emit our starting address
                    {
                        printf("%o/\n", cur_addr);
                    }

                    formatInstr(cur_addr, word);
                }
            }
            break;

        case RIM:
            if( OPERATION(word) == 060 )        // end of RIM code block
            {
                state = LOOKING;                // look for a BIN block now
                start_addr = OPERAND(word);
                if( !as_macro )
                {
                    printf("End of RIM loading, start address is %04o\n", start_addr);
                }

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
            else
            {
                // We didn't get what we expected, not a standard macro-generated tape.
                if( as_macro )
                {
                    fprintf(stderr,"Nonstandard binary tape, unterminated RIM block at tape location %d\n",
                        tape_loc - 3);
                    fprintf(stderr,"Terminating.\n");
                    fclose(fP);
                    exit(1);
                }
                else
                {
                    fprintf(stderr,"Nonstandard binary tape, unterminated RIM block at tape location %d.\n",
                        tape_loc - 3);
                    fprintf(stderr,"Dumping remaining data as random code.\n");

                    printf("Dumping remainder as random data.\n");
                    formatInstr(0, word);                           // include what we just saw
                    state = DATA;
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
                    printf("Tape  Addr  Raw    Lbl   Instruction\n");
                }

                checksum = word;            // initial checksum

                if( as_macro )
                {
                    printf("%o/\n", cur_addr);  // set addr for macro
                }

                word = getWord(fP, 2, state);         // shold be 'dio endaddr + 1'
                if( OPERATION(word) != 032 )          // not, so this is not in ddt bin format
                {
                    DIAGNOSTIC(
                "State LOOKING, got a DIO but next word was not one, bad BIN bloct at tape_location %d\n",
                        tape_loc = 3);

                    if( as_macro )                      // bail out
                    {
                        fprintf(stderr,
                    "Looking for a BIN block, but saw a non-standard block at tape location %d, terminating.\n",
                            tape_loc = 3);
                        fclose(fP);
                        exit(1);
                    }
                    else
                    {
                        printf("\nBeginning of non-BIN data\n");
                        formatInstr(cur_addr, 0320000 | cur_addr);  // the first word, a DIO.
                        ++cur_addr;
                        pushbackWord(word);         // so we don't lose it transitioning to state DATA

                        state = DATA;               // could be more on the tape, back to searching for RIM
                    }
                }
                else
                {
                    end_addr = OPERAND(word);       // last location + 1, in checksum as the dio instruction
                    checksum = addOnesComplement(checksum, word);
                    state = BIN;
                }
            }
            else if( OPERATION(word) == 060 )   // JMP, done loading BIN blocks, end of valid input
            {
                if( getLabel(OPERAND(word), OPERAND(word), labelStr) != -1 )
                {
                    printf("\n     start %s\n", labelStr); // macro directive to give start addr
                }
                else
                {
                    printf("\n     start %04o\n", OPERAND(word));
                }

                if( as_macro )
                {
                    did_start = true;
                    state = DONE;
                    DIAGNOSTIC("Saw jmp %04o at tape location %d, new state is DONE", OPERAND(word), tape_loc);
                }
                else
                {
                    formatInstr(cur_addr, word);            // emit the JMP
                    printf("\n");
                    state = DATA;
                    DIAGNOSTIC("Saw jmp %04o at tape location %d, new state is DATA", OPERAND(word), tape_loc);
                }
            }
            else
            {
                // Random data outside a RIM or BIN
                if( as_macro )
                {
                        fprintf(stderr,
                    "Looking for a RIM or BIN, but saw random binary at tape location %d, terminating.\n",
                        tape_loc + 3);

                        fclose(fP);
                        exit(1);
                }
                else
                {
                    // Just dump it with no address
                    formatInstr(0, word);
                }
            }
            break;

        case BIN:
            {
                // word will contain the 18 bit value for the current pc
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

        case RAW:
        case DATA:                                      // we got past the end of all BIN blocks, ignore the rest
            // word will contain the 18 bit value we read, dump it
            formatInstr(0, word);
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
CodeDef *instructionP;
char tmpstr[16];

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

            return;          // all done
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

                if( (word = getWord(fP, 1, state)) == -1 )
                {
                    return;                         // all done
                }

                if( keep_rim )
                {
                    markValidByInstruction(cur_addr, word);
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
                start_addr = OPERAND(word);

                if( keep_rim )
                {
                    markValidByInstruction(cur_addr, word);
                }

                DIAGNOSTIC("End of RIM, start address %04o\n", start_addr);
                DIAGNOSTIC("New state is LOOKING");

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
                if( keep_rim )
                {
                    markValidByInstruction(cur_addr, word);
                }
            }
            else
            {
                // We expectd an 032 or an 060, didn't get it.
                // This means the tape isn't a standard RIM/BIN produced by macro.
                // Just stop pass one, let pass two deal with it.
                DIAGNOSTIC("Unterminated RIM block at tape position %d\n", tape_loc);
                return;
            }
            break;

        case LOOKING:
            if( OPERATION(word) == 032 )   // RIM ended, DIO, beginning of BIN block
            {
                cur_addr = OPERAND(word);   // starting address
                DIAGNOSTIC("BIN start, addr %04o", cur_addr);

                checksum = word;            // initial checksum
                word = getWord(fP, 1, state);         // will be 'dio endaddr + 1'

                if( OPERATION(word) != 032 )        // should have been another DIO, give up
                {
                    return;                         // nothing more in pass one for the rest
                }

                checksum = addOnesComplement(checksum, word);
                end_addr = OPERAND(word);
                state = BIN;
                DIAGNOSTIC("New state is BIN");
            }
            else if( OPERATION(word) == 060 )   // JMP, done loading BIN blocks
            {
                DIAGNOSTIC("Found a JMP outside RIM or BIN at tape position %d\n", tape_loc - 3);
                markTarget(OPERAND(word));
                state = RESTART;                // could be more on the tape, back to searching for RIM
                DIAGNOSTIC("New state is RESTART");
            }
            break;

        case BIN:
            {
                // word will contain the 18 bit value for the current pc, add to checksum
                checksum = addOnesComplement(checksum, word);
                markValidByInstruction(cur_addr++, word);

                if( cur_addr >= end_addr )      // done, get the checksum from the tape, compare
                {
                    DIAGNOSTIC("End of BIN block");

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
}

// Perform 18-bit 1's complement additon, handling the carry-wraparound.
int
addOnesComplement(int a, int b)
{
    a += b;
    if( a & 01000000 )
    {
        a = (a & 0777777) + 1;                    // had a carry, add it back in 
    }

    return( a );
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
// If a word was pushed back, return it instead.
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

    if( saved_word != -1 )  // we had a pushed-back word
    {
        word = saved_word;
        saved_word = -1;
        return( word );
    }

    while( count )
    {
        if( (ch = fgetc(fP)) == EOF )
        {
            if( (count < 3) && (last_ch == 0377) )
            {
                DIAGNOSTIC("Saw 0377 then EOF, done");
                return( -1 );                  // Stop marker? 
            }

            if( (state != DATA) && (state != RAW) && (count != 3) )
            {
                fprintf(stderr,"Premature EOF at tape position %d\n", tape_loc);
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

void
pushbackWord(int word)
{
    saved_word = word;
}

Special *
findSpecial(Special *specP, int op)
{
int i;

    for( ;; )
    {
        if( specP->modifiers == UNKNOWN )
        {
            break;                          // should be end of list
        }

        if( specP->value == (op & specP->mask) )   // found it
        {
            break;
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
int bits03;
char *cP;
char tmpstr[32];
char tmpstr2[32];
CodeDef *instructionP;
Special *sP;

    if( getLabel(pc, pc, labelStr) != -1 )
    {
        if( as_macro )
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

    if( as_macro )
    {
        printf("%s", (labelStr[0] != '\0')?labelStr:"     ");
    }
    else
    {
        printf("%-5d %04o: %06o %s", tape_loc, pc, word, (labelStr[0] != '\0')?labelStr:"     ");
    }

    opcode = OPERATION(word);
    indirect = opcode & 01;
    opcode >>= 1;                                           // convert to 32 possible instructions

    operand = OPERAND(word);
    instructionP = &opcodes[opcode];

    if( instructionP->modifiers == IS_ILLEGAL )            // not an instruction, just emit the octal value
    {
        printf(" %06o\n", word);
        return;
    }

    switch( instructionP->modifiers )
    {
    case NONE:
        printf(" %s", instructionP->name);
        break;

    case CAN_INDIRECT:
        getLabel(operand, operand, labelStr);
        printf(" %s%s %s", instructionP->name, (indirect)?" i":"", labelStr);
        break;

    case IS_CALJDA:
        if( indirect )
        {
            getLabel(operand, operand, labelStr);
            printf(" jda %s", labelStr);
        }
        else
        {
            printf(" %s", instructionP->name);            // CAL
        }
        break;

    case IS_LAW:
        // The versions of macro1 floating arund are broken, they don't hande the 'law -n' syntax properly.
        //printf(" %s %s%04o", instructionP->name, (indirect)?"-":"", operand);
        printf(" %s %s%04o", instructionP->name, (indirect)?"i ":"", operand);
        if( !as_macro )
        {
            printf(" (%d dec)", (indirect)?-operand:operand);
        }
        break;

    case IS_SHIFT:
        tmp = ((indirect?010000:0) | operand) & 017000;      // indirect bit controls left-right
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

            printf(" %s",  sP->name);
            if( tmp2 != 0 )
            {
                printf("   %3ds", tmp2);        // only if we'e actually shifting
            }
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
        // Macro1 allows an 'or' operation, 'a!b', which is used in macro mode.
        // However, the native assembler doesn't support this, so any combined operations
        // are emitted as the octal word with a comment.
        // If we see any extra bits not valid microinstuctions, then just the octal data is output.
        tmp = 0;                // set to 1 if we have printed one already
        tmpstr[0] = 0;          // be sure we start empty
        printf(" ");

        // A special case is nothing set, means nop
        if( (operand & OPR_MASK_NOP) == 0 )
        {
            operand = 0;            // nothing left
            strcpy(tmpstr, "nop");
        }
        else
        {
            bits03 = operand & 07;              // needed for a few ops
            operand &= 07770;

            if( (operand & OPR_MASK_LAT) == OPR_MASK_LAT )    // MUST come befoe CLA! Only 2 bit directive.
            {
                operand &= ~OPR_MASK_LAT;
                sprintf(tmpstr2,"%slat", tmp?separator:"");
                strcat(tmpstr, tmpstr2);
                tmp = 1;
            }
            if( operand & OPR_MASK_CLA )        // MUST come after LAT!
            {
                operand &= ~OPR_MASK_CLA;
                sprintf(tmpstr2,"%scla", tmp?separator:"");
                strcat(tmpstr, tmpstr2);
                tmp = 1;
            }
            if( operand & OPR_MASK_CLI )
            {
                operand &= ~OPR_MASK_CLI;
                sprintf(tmpstr2,"%scli", tmp?separator:"");
                strcat(tmpstr, tmpstr2);
                tmp = 1;
            }
            if( operand & OPR_MASK_CMA )
            {
                operand &= ~OPR_MASK_CMA;
                sprintf(tmpstr2,"%scma", tmp?separator:"");
                strcat(tmpstr, tmpstr2);
                tmp = 1;
            }
            if( operand & OPR_MASK_HLT )
            {
                operand &= ~OPR_MASK_HLT;
                sprintf(tmpstr2,"%shlt", tmp?separator:"");
                strcat(tmpstr, tmpstr2);
                tmp = 1;
            }
            if( !(operand & OPR_MASK_STF) && bits03 )
            {
                // CLF is bits03, already cleared
                sprintf(tmpstr2,"%sclf %o", tmp?separator:"", bits03);
                strcat(tmpstr, tmpstr2);
                bits03 = 0;                  // done with these
                tmp = 1;
            }
            if( (operand & OPR_MASK_STF) && bits03 )
            {
                operand &= ~OPR_MASK_STF;
                sprintf(tmpstr2,"%sstf %o", tmp?separator:"", bits03);
                strcat(tmpstr, tmpstr2);
                bits03 = 0;                  // done with these
                tmp = 1;
            }
        }

        if( compatibility_mode && (tmp == 1) )                  // multi component
        {
            if( operand | bits03 )                              // extra bits set
            {
                printf("%06o", word);
            }
            else
            {
                printf("%06o / %s", word, tmpstr);
            }
        }
        else if( (operand | bits03) != 0 )                      // extra bits were left over, must be data
        {
            printf("%06o", word);
        }
        else
        {
            printf("%s", tmpstr);
        }
        break;

    case IS_IOT:
        // Sometimes there are extra bits in the operand above the usual 6 bits
        tmp2 = operand & 037700;

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

                printf("%s%05o", (as_macro)?separator:" | ", operand);

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

            tmp2 = 0;              // dpy uses some of the special bits
            break;

        case HAS_ID:            // some encode a subdevice, eg tape drive number
            printf(" %02o", (operand >> 6) & 077);
            tmp2 = 0;
            break;
        }

        if( (sP->modifiers != UNKNOWN) && (tmp2 != 0) )
        {
            printf("%s%04o", (as_macro)?separator:" | ", tmp2);   // add extra bits
        }
        break;
    }

    printf("\n");
}

// Set memory location as used, i.e. was loaded by loader
void
markValid(int address)
{
    if( compatibility_mode )
    {
        return;                         // we don't support labels
    }

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
CodeDef *instructionP;

    if( compatibility_mode )
    {
        return;                         // we don't support labels
    }

    markValid(addr);            //this address is used

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
int itmp, itmp2;
char ch;
char *cP;

    if( compatibility_mode )
    {
        return;                         // we don't support labels
    }

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

void usage()
{
    fprintf(stderr,"Usage: disassemble_tape [-midkcr] filename\n");
    fprintf(stderr,"where:\n");
    fprintf(stderr,"m - output in pure macro assember form\n");
    fprintf(stderr,"i - print any unknown IOTs on stderr\n");
    fprintf(stderr,"d - enable diagnostics for debugging this progam\n");
    fprintf(stderr,"k - keep RIM loader code if seen and in macro mode; normally no because MACRO usually adds it\n");
    fprintf(stderr,"c - compatibility with native assembler mode\n");
    fprintf(stderr,"r - raw mode, just dump every binary word as an instruction, no RIM or BIN checking\n");
    fprintf(stderr,"    raw verrides all other flags except d\n");
    fprintf(stderr,"Flags can be together, -mid, or separate, -m -i -d\n");
    exit(1);
}
