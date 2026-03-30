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
 * The third mode is raw mode, each 18 bit data word is just printed as a an octal value.
 *
 * The PDP-1 binary tapes are loaded initially using read-in, which ignores any character that doesn't have
 * bit 0200 set.
 * However, many tapes have a leader punched that shows a punch pattern that makes descriptive text.
 * This is also captured by printing out in 'readable label' form any character up to the first 0200 one.
 *
 * Processing then continues looking for a RIM block, followed by ddt-form BIN blocks or if -a is given,
 * am1 extended loader blocks.
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
 * Am1 uses its own loader to deal with extended memory:
 *
 * 7751: 724074          eem            if extended memory was not used, am1 will replace this with a nop
 * 7752: 730002      loop, rpb          no checksum is done, we aren't reading from a pysical tape reader
 * 7753: 327773          dio addr       word is the address to being storing data or negative if done
 * 7754: 642000          spi
 * 7755: 607766          jmp done
 * 7756: 730002          rpb            word is the ending address + 1
 * 7757: 327774          dio end
 * 7760: 730002      load, rpb          read data words and store until end is reached
 * 7761: 337773          dio i addr
 * 7762: 447773          idx addr
 * 7763: 527774          sas end
 * 7764: 607760          jmp load
 * 7765: 607752          jmp loop
 * 7766: 662001      done, ril 1s       the last word read in loop, above, is the start address
 * 7767: 652000          spi i          if bit 0 was set....
 * 7770: 617773          jmp i addr     start prog
 * 7771: 760400          hlt            nostart, just halt
 * 7772: 607752          jmp loop       and go again
 * 7773: 000000      addr, 0
 * 7774: 000000      end, 0
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
 * 18/12/2025 wje - Added support for the new AM1 loader.
 * 06/01/2026 wje - Added support for pause in the AM1 loader.
 * 09/02/2026 wje - Fix completion bit handling, dpy i and c handling
 * 20/02/2026 wje - If not an instruction, be sure to emit all bits
 * 01/03/2026 wje - Fix cal, emit operand if it actually isn't a cal
 * 15/03/2026 wje - Change to use decode_instruction, no reason to duplicate all the code
 * 19/03/2026 wje - Pass macro flag to decodeInstr()
 * 30/03/2026 wje - Major rework for full multi-bank support in am1, fix some bugs, drop compatiiblity mode, -c
 *
 */
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <libgen.h>

#define DIAGNOSTIC(args...) if( diagnostics ) {printf(args); printf("\n");}

#define MEMSIZE 4096    // words in a memory bank
#define BANKS 16        // and the number of banks we support

#define BANKOF(a) ((a) & ~(MEMSIZE - 1))   // get the bank part of a full 16 bit address, 4 bits
#define BANKNUM(a) (BANKOF(a) >> 12)       // get the bank number of a full 16 bit address

// Instructions have a 5 bit opcode followed by a 1 bit indirect marker as the high 6 bits of a word
// IOTs can also have a completion-requested, bit 6 set, 04000.
#define OPERATION(x)    (((x) >> 12) & 076)
// The remaining 12 low bits are the operand whose meaning varies by instruction
#define OPERAND(x)      (x & 0007777)

// The processing loop is a simple state machine
typedef enum {START, RESTART, LOOKING, RIM, BIN, DATA, RAW, DONE} State;

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
char *stateToName(State state);

extern char *decodeInstr(int word, int addr, bool asMacro, char *separatorP, char *symbolP, char *resultP);
extern bool opCanIndirect(int opcode);

// Set from cmd line args
bool as_macro = false;
bool raw_mode = false;
bool show_leader = true;
bool diagnostics = false;
bool keep_rim = false;
bool verbose = true;
bool am1Loader = false;

int pass = 1;               // first pass
int curBank = 0;            // used for am1
int lastAddr = 0;;
int label_number = 1;       // used with memlocs and labels
int tape_loc = 0;
int checksum = 0;
int saved_word = -1;        // for getWord() pushback
State state;
char separator[4];          // used in macro mode

char labelStr[16];          // for formatting a label

// This array is used for tracking labels.
// The low 9 bits are the label number.
// It is indexed by a memory address.
Label memlocs[BANKS * MEMSIZE];       // enough for the entire address space

#define MEM_VALID  01       // this location was seen as a load address in RIM or BIN
#define MEM_TARGET 02       // this location was seen as the target of a memoery reference

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
            case 'a':
                am1Loader = true;
                break;

            case 'm':
                as_macro = true;
                verbose = false;
                break;

            case 'd':
                diagnostics = true;
                break;

            case 'k':
                keep_rim = true;
                break;

            case 'l':
                show_leader = false;
                break;

            case 'r':
                raw_mode = true;
                break;

            case 'v':
                verbose = false;
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

    if( !raw_mode )
    {
        passOne(fP);                // first pass finds all locations that need labels and validates the tape.
        fclose(fP);
        fP = fopen(filename, "r"); // reopen for second pass
        tape_loc = 0;               // reset tape position
    }

    if( as_macro && !raw_mode )
    {
        strcpy(shortname, basename(filename));

        // sorry, can't have a dot.
        if( (cP = strrchr(shortname, '.')) )
        {
            *cP = '\0';
        }

        printf("Disassembled from %s\n", shortname);
        printf("ioh=iot i\n");   // just makes things easier
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

                if( verbose && !as_macro && keep_rim )
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
                if( !as_macro && keep_rim )
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
            if( am1Loader )
            {
                if( word & 0600000 )
                {
                    // am1 loader end-of-code, start addr or pause
                    if( (word & 0600000) == 0600000 )
                    {
                        printf("\n     pause\n");
                        DIAGNOSTIC("Saw pause at tape location %d, new state is DONE", tape_loc);
                        state = DONE;
                    }
                    else
                    {
                        word &= 0177777;
                        printf("\n     start 0%06o\n", word);    // could be an extended address

                        if( as_macro )
                        {
                            did_start = true;
                            state = DONE;
                            DIAGNOSTIC("Saw jmp %06o at tape location %d, new state is DONE", word, tape_loc);
                        }
                        /*
                        else
                        {
                            word2 = 0617770;
                            formatInstr(cur_addr, word);            // emit the JMP
                            printf("\n");
                            state = DATA;
                            DIAGNOSTIC("Saw jmp %04o at tape location %d, new state is DATA",
                                OPERAND(word), tape_loc);
                        }
                        */
                    }
                }
                else
                {
                    cur_addr = word;        // start of am1 block
                    end_addr = getWord(fP, 2, state);
                    state = BIN;
                }
            }
            else if( OPERATION(word) == 032 )   // RIM ended, DIO, beginning of BIN block
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

                    if( as_macro )
                    {
                        // Print as a comment
                        printf("/ %06o\n", word);
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
                        // Print as a comment
                        printf("/ %06o\n", word);
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
                // cur_addr will be the full 16 bit address for am1
                formatInstr(cur_addr++, word);

                if( cur_addr >= end_addr )      // done, get the checksum from the tape, compare
                {
                    if( verbose && !as_macro  )
                    {
                        printf("End of %s block at tape position %d\n", am1Loader?"AM1":"BIN", tape_loc - 3);
                    }

                    if( !am1Loader )
                    {
                        word = getWord(fP, 2, state);         // checksum already checked in pass one
                    }
                    state = LOOKING;
                }
            }
            break;

        case RAW:
        case DATA:                                      // we got past the end of all BIN blocks, ignore the rest
            // word will contain the 18 bit value we read, dump it
            if( state == RAW )
            {
                printf("0%06o\n", word);
            }
            else
            {
                formatInstr(0, word);
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
            if( start_addr >= MEMSIZE ) // this code used a bank start address outside of bank 0
            {
                printf("/ Warning - original start was %o, not in bank 0.\n", start_addr);
            }
            printf("     start %o\n", start_addr & 07777); // macro directive to give start addr
        }
    }
    else if( verbose )
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
            if( am1Loader )
            {
                if( word & 0600000 )
                {
                    // am1 loader end-of-code, start addr or pause
                    return;
                }
                else
                {
                    cur_addr = word;        // start of am1 block
                    end_addr = getWord(fP, 2, state);
                    state = BIN;
                }
            }
            else if( OPERATION(word) == 032 )   // RIM ended, DIO, beginning of BIN block
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
                if( !as_macro )
                {
                    if( verbose && show_leader && saw_space )
                    {
                        printf("\n");
                        saw_space = 0;
                    }

                    if( verbose && show_leader )
                    {
                        printTapeLeader(ch);
                    }
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
int tmp;
int bank;
bool needBank = false;
char symbolstr[256];
char tmpstr[256];

    if( BANKNUM(pc) != curBank )
    {
        if( as_macro )
        {
            printf("/ WARNING - this code uses extended memory, it will not work properly!\n");
        }

        needBank = true;
        curBank = BANKNUM(pc);
    }

    if( getLabel(pc, pc, labelStr) != -1 )
    {
        if( !verbose || as_macro  )
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

    if( !verbose )
    {
        if( needBank )
        {
            printf((as_macro)?"/ Now in bank %o\n":"bank %o\n", curBank);
        }

        if( pc != (lastAddr + 1) )
        {
            printf("%o/\n", pc & 07777);    // addr is relative to current bank
        }

        lastAddr = pc;

        printf("%s", (labelStr[0] != '\0')?labelStr:"     ");
    }
    else
    {
        if( needBank )
        {
            printf("%-5d %04o: bank %o\n", tape_loc, pc, curBank);
        }

        printf("%-5d %04o: %06o %s", tape_loc, pc, word, (labelStr[0] != '\0')?labelStr:"     ");
    }

    tmp = OPERATION(word) >> 1;
    if( opCanIndirect(tmp) )
    {
        tmp = BANKOF(pc) | (word & 07777);
        getLabel(tmp, word & 07777, symbolstr);
    }
    else
    {
        symbolstr[0] = '\0';
    }

    decodeInstr(word, word & 07777, as_macro, separator, symbolstr, tmpstr);
    printf("%s\n", tmpstr);
}

// Set memory location as used, i.e. was loaded by loader
void
markValid(int address)
{
int bank;

    address &= (BANKS * MEMSIZE) - 1 ; // for safety, limit to our supported size
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

    opcode = OPERATION(word) >> 1;
    operand = OPERAND(word);

    if( opCanIndirect(opcode) )
    {
        operand = BANKOF(addr) | operand;
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

    address &= (BANKS * MEMSIZE) - 1 ; // for safety, limit to our supported size

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

    address &= (BANKS * MEMSIZE) - 1 ; // for safety, limit to our supported size

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

char *
stateToName(State state)
{
    switch( state )
    {
    case START:
        return("START");
    case RESTART:
        return("RESTART");
    case LOOKING:
        return("LOOKING");
    case RIM:
        return("RIM");
    case BIN:
        return("BIN");
    case DATA:
        return("DATA");
    case RAW:
        return("RAW");
    case DONE:
        return("DONE");
    default:
        return("UNKNOWN");
    }
}

void
usage()
{
    fprintf(stderr,"Usage: disassemble_tape [-amklrd] filename\n");
    fprintf(stderr,"where:\n");
    fprintf(stderr,"a - expect the am1 loader\n");
    fprintf(stderr,"m - output in pure macro assember form, warn about extended memory use\n");
    fprintf(stderr,"k - keep RIM loader code if seen and in macro mode; normally no because MACRO usually adds it\n");
    fprintf(stderr,"l - output the leader in readable form, only in verbose\n");
    fprintf(stderr,"r - raw mode, just dump every binary word as an instruction, no RIM or BIN checking\n");
    fprintf(stderr,"d - enable diagnostics for debugging this progam\n");
    fprintf(stderr,"    raw overrides all other flags except d\n");
    fprintf(stderr,"Flags can be together, -mid, or separate, -m -i -d\n");
    exit(1);
}
