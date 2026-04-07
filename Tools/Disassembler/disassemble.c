/*
 * Disassemble a punched tape that has RIM and/or binary load images supported by a defined loader.
 *
 * See comments on this source below.
 *
 * This program disassembles PDP-1 binary images into macro assembly or am1 instructions using
 * the loader type given.
 * The default loader is 'bin', the typical DEC DDT loader, digital-1-3-s-mb_ddt.bin.
 *
 * Four modes are supported:
 *
 * The default mode is a verbose disassembly with detailed information about the disassembled content, but it cannot
 * be assembled.
 *
 * The second mode is macro1 mode, which will generate output that can be assembled by the macro1 and variant
 * assemblers. It will generate labels for referenced locations.
 * It cannot be assembled by the native PDP-1 assembler.
 * If code is in extended memory, the code can be assembled but will not work.
 * Macro1 does not support extended memory.
 * Comments will be emitted to note this.
 * The output can also be assembled by am1.
 *
 * The third mode is am1 mode, which will generate output that can be assembled by the am1 assembler.
 * This can frequently also be assembled by macro1, but only if no extended memory is used.
 * The only difference is when extended memory is used.
 * In that case, the am1 bank directives will be emitted.
 *
 * The fourth mode is raw mode, each 18 bit data word is just printed as a an octal value or as a decoded
 * instruction.
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
 * 22-Sep-2025 wje - Initial version
 * 23-Sep-2025 wje - Convert to two pass
 * 24-Sep-2025 wje - Make macro-style formatting of labels nicer, fixes for 'instructions' that are actually data
 * 24-Sep-2025 wje - Add raw mode for tapes that don't have a standard loader, just dump everything as instructions
 * 25-Sep-2025 wje - Various fixes around OPR and such.
 *                  IMPORTANT - assemblers, native and cross, are broken for law -n,
 *                  so if a binary for 'law -n' is seen generate 'safe' law i n which will give the correct result.
 *                  The behavior of law -n does not match the original DEC Macro documentation.
 *                  Instead of law -n generating effectively law i n, the actual negative number is added to the
 *                  law opcode, producing garbage.
 * 25-Sep-2025 wje - Add a -s switch to generate 3 char labels for backwards compatibility
 * 27-Sep-2025 wje - Continue to process a tape even if a malformed BIN block is found, jsut give a warning.
 * 28-Sep-2025 wje - Add -c for compalibility mode, will emit source that works with the native PDP-1 macro assembler.
 * 03-Oct-2025 wje - Handle non-standard tapes better, bail if in macro mode, dump in default mode.
 * 18-Dec-2025 wje - Added support for the new AM1 loader.
 * 06-Jan-2026 wje - Added support for pause in the AM1 loader.
 * 09-Feb-2026 wje - Fix completion bit handling, dpy i and c handling
 * 20-Feb-2026 wje - If not an instruction, be sure to emit all bits
 * 01-Mar-2026 wje - Fix cal, emit operand if it actually isn't a cal
 * 15-Mar-2026 wje - Change to use decode_instruction, no reason to duplicate all the code
 * 19-Mar-2026 wje - Pass macro flag to decodeInstr()
 * 30-Mar-2026 wje - Major rework for full multi-bank support in am1, fix some bugs, drop compatiiblity mode, -c
 * 02-Apr-2026 wje - Another major rework for pluggable loaders, now version 2.0
 * 05-Apr-2026 wje - Add symbol import file
 * 06-Apr-2026 wje - Add -n flag to eliminate multiple outputs of an address
 *
 */

#define VERSION 2.3

#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <libgen.h>

#include "loader.h"
#include "decode_instruction.h"

#define DIAGNOSTIC(enable, args...)
//#define DIAGNOSTIC(enable, args...) if( enable ) {fprintf(stderr, args); fprintf(stderr, "\n");}
// Set desired ones to non-zero
#define DIAG_PASS 0
#define DIAG_STATE 1
#define DIAG_LDR 0
#define DIAG_ERR 0

#define MEMSIZE 4096    // words in a memory bank
#define BANKS 16        // and the number of banks we support

#define BANKOF(a) ((a) & ~(MEMSIZE - 1))   // get the bank part of a full 16 bit address, 4 bits
#define BANKNUM(a) (BANKOF(a) >> 12)       // get the bank number of a full 16 bit address

#define LABELSIZE 32    // max symbol length + 1

// Instructions have a 5 bit opcode followed by a 1 bit indirect marker as the high 6 bits of a word
// IOTs can also have a completion-requested, bit 6 set, 04000.
#define OPERATION(x)    (((x) >> 12) & 076)
// The remaining 12 low bits are the operand whose meaning varies by instruction
#define OPERAND(x)      (x & 0007777)

// Used for label tracking
#define MEM_USED     0x1       // this location was seen as a load address in RIM or BIN
#define MEM_TARGET   0x2       // this location was seen as the target of a memory reference
#define MEM_MODIFIED 0x4       // this location was written to
#define MEM_LOADED   0x8       // this location loaded, not just referenced
#define MEM_EMITTED  0x10      // this location's label has been printed

// convert a bank-relative address to an absolute address
#define FULLADDR(x) ((curBank << 12) | (x))

// The processing loop is a simple state machine
typedef enum {START, RESTART, LOADING, RIM, DATA, RAW, DONE} State;

// This is used to hold label information.
// The location in the label array is the address of this label.
typedef struct
{
    int flags;          // the MEM_x flags
    int instFlags;      // the INSTR_x flags from decodeInstr(), if any
    int refAddr;        // the address of whatever caused this label to be created
    char label[LABELSIZE];
} Label, *LabelP;

#include "loaderdefs.h"

bool skipRim = false;
bool asMacro = false;
bool asAm1 = false;
bool rawMode = false;
bool showLeader = false;
bool allowRepeat = true;
bool keepRim = false;
bool verbose = true;
bool extendedMem = false;

int pass = 1;               // first pass
int curBank = 0;            // used for am1
int lastAddr = 0;;
int label_number = 1;       // used with memlocs and labels
int var_number = 1;         // used with memlocs and labels
int const_number = 1;       // used with memlocs and labels
int tape_loc = 0;
int saved_word = -1;        // for getWord() pushback
FILE *outfP;
State state;
LoaderP loaderP;
char separator[4];          // used in macro mode

// This is a bitmap for tracking used memory locations.
// Each bit represents one word in memory.
uint64_t memMap[(BANKS * MEMSIZE) / sizeof(uint64_t)];

// This array is used for tracking labels.
// The low 9 bits are the label number.
// It is indexed by a memory address.
LabelP memlocs[BANKS * MEMSIZE];       // enough for the entire address space

int getWord(FILE *, int, int);
int nextWord(FILE *);
LabelP getLabelPointer(int address, bool create);
int getLabel(int, int,  char*);
char *parseLoader(char *strP, char *args[]);
LoaderP findLoader(LoaderMap loaders[], char *nameP);
void pushbackWord(int);
void formatInstr(int, int);
void generateLabels(void);
void printTapeLeader(int);
void passOne(FILE *infP, FILE *outfP);
LabelP markValid(int addr, int iFlags);
void markValidByInstruction(int addr, int word);
void markTarget(int addr, int creator, bool modified);
void checkForVariables(FILE *outfP, int addr, int endAddr);
void showLoaders();
bool loadSymbols(char *filenameP);
bool setBit(uint64_t map[], int addr);
bool isBitSet(uint64_t map[], int addr);
void usage(void);

extern bool opCanIndirect(int opcode);
extern char *decodeInstr(int word, int addr, bool asMacro, char *separatorP,
    char *symbolP, char *resultP, int *flagsP);

int
main(int argc, char **argv)
{
int opt;
int word;
int curAddr;
int endAddr;               // for BIN loader
char *cP;
FILE *fP;
char filename[256];
char shortname[256];
char tmpstr[128];
char *ldrArgs[128];

    strcpy(separator, "!");         // is a logical or in macro1
    outfP = stdout;
    loaderP = binloader;

    // parse our comd line args
    while( (opt = getopt(argc, argv, "adklmnrv?L:s:o:")) != -1 )
    {
        switch( opt )
        {
        case '?':
            showLoaders();
            exit(0);
            break;

        case 'a':
            asAm1 = true;
            asMacro = false;
            verbose = false;
            break;

        case 'm':
            asMacro = true;
            asAm1 = false;
            verbose = false;
            break;

        case 'k':
            keepRim = true;
            break;

        case 'l':
            showLeader = true;
            break;

        case 'n':
            allowRepeat = false;
            break;

        case 'r':
            rawMode = true;
            break;

        case 'v':
            verbose = true;
            asMacro = false;
            asAm1 = false;
            break;

        case 'L':
            if( !(cP = parseLoader(optarg, ldrArgs)) )
            {
                fprintf(stderr,"At least a loader name is required for -L.\n");
                exit(1);
            }

            if( !(loaderP = findLoader(loaders, cP)) )
            {
                fprintf(stderr,"No such loader '%s' is known.\n", optarg);
                exit(1);
            }
            break;

        case 's':
            if( !loadSymbols(optarg) )
            {
                fprintf(stderr,"Can't load sybol file '%s', ignored.\n", optarg);
            }
            break;

        case 'o':
            if( !(outfP = fopen(optarg, "w")) )
            {
                fprintf(stderr,"Can't open output file '%s'\n", optarg);
                exit(1);
            }
            break;
        }
    }

    if( optind == (argc - 1) )
    {
        if( !(fP = fopen(argv[optind], "r")) )
        {
            fprintf(stderr,"Can't open input file '%s'\n", argv[optind]);
            exit(1);
        }

        strcpy(filename, argv[optind]);
    }
    else
    {
        usage();                // should only be the input filename
    }

    // Pass any loader initialization args to the loader.
    // Any word value is ignored.
    if( (opt = (*loaderP)(outfP, LOADER_CMD_INIT, &word, &word, ldrArgs)) == LOADER_ERROR )
    {
        fprintf(stderr, "The loader has reported an error during initialization.\n");
        fclose(outfP);
        exit(1);
    }

    if( opt & LOADER_INIT_NORIM )
    {
        skipRim = true;
    }

    if( !rawMode )
    {
        DIAGNOSTIC(DIAG_PASS, "Pass one started");
        passOne(fP, outfP);         // first pass finds all locations that need labels and validates the tape.
        generateLabels();
        fseek(fP, 0, SEEK_SET);
        tape_loc = 0;               // reset tape position
    }

    if( asMacro && !rawMode )
    {
        strcpy(shortname, basename(filename));

        // sorry, can't have a dot.
        if( (cP = strrchr(shortname, '.')) )
        {
            *cP = '\0';
        }

        fprintf(outfP,"Disassembled from %s\n", shortname);
        fprintf(outfP,"ioh=iot i\n");   // just makes things easier
    }
    else
    {
        fprintf(outfP,"Disassembled from %s\n", filename);
    }

    // We don't check for errors because those were already detected by pass one
    state = (rawMode)?RAW:START;
    DIAGNOSTIC(DIAG_PASS, "Pass two started");

    curAddr = 0;

    if( skipRim )
    {
        state = LOADING;            // go right to loading
        if( (*loaderP)(fP, LOADER_CMD_START, &word, &word, NULL) == LOADER_ERROR )
        {
            fprintf(stderr,"Loader returned an error at read location %d\n", tape_loc);
            fprintf(stderr,"Terminating.\n");
            fclose(fP);
            exit(1);
        }
    }

    // The state machine loop
    for(;;)
    {
        if( state == DONE )
        {
            break;
        }

        if( state != LOADING )      // loaders do their own reading
        {
            word = getWord(fP, 2, state);

            if( word == -1 )
            {
                break;          // all done
            }
        }

        DIAGNOSTIC(DIAG_STATE, "State in pass 2 now %d", state);
        switch( state )
        {
        case START:
        case RESTART:                               // only difference is that we don't print leader info
            lastAddr = 0;
            if( OPERATION(word) == 032 )            // beginning of the RIM code block
            {
                state = RIM;
                curAddr = OPERAND(word);

                if( verbose && keepRim )
                {
                    fprintf(outfP,"Start of RIM block at tape position %d\n", tape_loc - 3);
                    fprintf(outfP,"Tape  Addr  Raw    Lbl   Instruction\n");
                }

                word = getWord(fP, 2, state);
                if( keepRim )
                {
                    formatInstr(curAddr, word);
                }
            }
            break;

        case RIM:
            if( OPERATION(word) == 060 )        // end of RIM code block
            {
                state = LOADING;                // loader-specific block now
                // Reset our address to 0, initial condition, and restart the loader
                lastAddr = curAddr = 0;
                if( (*loaderP)(fP, LOADER_CMD_START, &word, &word, NULL) == LOADER_ERROR )
                {
                    fprintf(stderr,"Loader returned an error at read location %d\n", tape_loc);
                    fprintf(stderr,"Terminating.\n");
                    fclose(fP);
                    exit(1);
                }
            }
            else if( OPERATION(word) == 032 )   // next data word to load
            {
                lastAddr = curAddr = OPERAND(word);
                word = getWord(fP, 2, state);

                if( keepRim )
                {
                    formatInstr(curAddr, word);
                }
            }
            else
            {
                // We didn't get what we expected, not a standard macro-generated tape.
                if( asMacro )
                {
                    fprintf(stderr,"Nonstandard binary tape, unterminated RIM block at read location %d\n",
                        tape_loc - 3);
                    fprintf(stderr,"Terminating.\n");
                    fclose(fP);
                    exit(1);
                }
                else
                {
                    fprintf(stderr,"Nonstandard binary tape, unterminated RIM block at read location %d.\n",
                        tape_loc - 3);
                    fprintf(stderr,"Dumping remaining data as random code.\n");

                    formatInstr(0, word);                           // include what we just saw
                    state = DATA;
                }
            }
            break;

        case LOADING:
            // curAddr will be the full 16 bit address for am1
            // We have to check for referenced data that isn't included in the tape data, macro1
            // does not emit anything for variables, it just skips the memory area.
            switch( (*loaderP)(fP, LOADER_CMD_NEXT, &curAddr, &word, NULL) )
            {
            case LOADER_DONE:
                state = DONE;

                if( asMacro && extendedMem && (BANKOF(curAddr) != 0) )
                {
                    fprintf(outfP,"/ Warning - start address not in bank 0.\n");
                }

                // Put out the start directive
                if( verbose )
                {
                    fprintf(outfP,"%-5d %06o: 000000", tape_loc, lastAddr);
                }

                if( getLabel(curAddr, curAddr, tmpstr) == -1 )
                {
                    sprintf(tmpstr,"%06o", curAddr);
                }
                else if( asAm1 )
                {
                    word = BANKNUM(curAddr);
                    if( word )
                    {
                        fprintf(outfP,"     start %s:%o\n", tmpstr, word);
                    }
                    else
                    {
                        fprintf(outfP,"     start %s\n", tmpstr);
                    }
                }
                else
                {
                    fprintf(outfP,"     start %s\n", tmpstr);
                }

                state = DONE;
                break;

            case LOADER_STOP:
                if( asMacro )
                {
                    if( extendedMem ) // this code used a bank start address outside of bank 0
                    {
                        fprintf(outfP,"/ Warning - program used memory not in bank 0.\n");
                    }

                    fprintf(outfP,"/ Warning - program ended with an am1 stop, no start address available.\n");
                }
                else
                {
                    fprintf(outfP,"%-5d %06o: 000000      stop\n", tape_loc, lastAddr);
                }
                state = DONE;
                break;

            case LOADER_AGAIN:
            case LOADER_OK:
                continue;

            case LOADER_MORE:
                if( curAddr >> 12 )
                {
                    extendedMem = true;
                }

                if( allowRepeat || setBit(memMap, curAddr) )
                {
                    if( curAddr != (lastAddr + 1) )
                    {
                        checkForVariables(outfP, lastAddr, curAddr);
                    }

                    formatInstr(curAddr, word);
                }

                lastAddr = curAddr;
                break;

            case LOADER_ERROR:
                fprintf(stderr,"Loader returned an error at read location %d\n", tape_loc);
                fprintf(stderr,"Terminating.\n");
                fclose(fP);
                exit(1);
            }
            break;

        case RAW:
        case DATA:                                  // we got past the end of all BIN blocks, dump the rest
            // word will contain the 18 bit value we read, dump it
            if( state == RAW )
            {
                if( asMacro || asAm1 )
                {
                    formatInstr(curAddr++, word);
                }
                else
                {
                    fprintf(outfP,"0%06o\n", word);
                }
            }
            break;

        default:
            fprintf(outfP,"Bad state %d\n", state);
            break;
        }
    }

    if( verbose )
    {
        fprintf(outfP,"\nDone\n");
    }

    fclose( fP );
    exit(0);
}

void
passOne(FILE *fP, FILE *outfP)
{
int word;
int curAddr;
int endAddr;               // for BIN loader

    if( skipRim )
    {
        state = LOADING;            // go right to loading
        if( (*loaderP)(fP, LOADER_CMD_START, &word, &word, NULL) == LOADER_ERROR )
        {
            fprintf(stderr,"Loader returned an error at read location %d\n", tape_loc);
            fprintf(stderr,"Terminating.\n");
            fclose(fP);
            exit(1);
        }
    }
    else
    {
        state = START;
    }

    DIAGNOSTIC(DIAG_PASS, "Starting pass one");

    // The state machine loop
    for(;;)
    {
        DIAGNOSTIC(DIAG_STATE, "State in pass 1 now %d", state);
        switch( state )
        {
        case START:
        case RESTART:                               // only difference is that we don't print leader info
            word = getWord(fP, 1, state);
            if( OPERATION(word) == 032 )            // beginning of the RIM code block
            {
                state = RIM;
                DIAGNOSTIC(DIAG_STATE, "New state is RIM");
                pushbackWord(word);
            }
            else
            {
                fprintf(stderr,"Binary word %06o at tape position %d before RIM block, invalid input.\n",
                    word, tape_loc);
                exit(1);
            }
            break;

        case RIM:
            word = getWord(fP, 1, state);
            if( OPERATION(word) == 060 )        // end of RIM code block
            {
                if( keepRim )
                {
                    markValidByInstruction(curAddr, word);
                }

                DIAGNOSTIC(DIAG_STATE, "New state is LOADING");

                state = LOADING;                // pass off to laoder
                // Reset our address to 0, initial condition
                curAddr = 0;
                if( (*loaderP)(fP, LOADER_CMD_START, &word, &word, NULL) == LOADER_ERROR )
                {
                    fprintf(stderr,"Loader returned an error at read location %d\n", tape_loc);
                    fprintf(stderr,"Terminating.\n");
                    fclose(fP);
                    exit(1);
                }
            }
            else if( OPERATION(word) == 032 )   // next data word to load
            {
                curAddr = OPERAND(word);

                word = getWord(fP, 1, state);
                if( keepRim )
                {
                    markValidByInstruction(curAddr, word);
                }
            }
            else
            {
                // We expectd an 032 or an 060, didn't get it.
                // This means the tape isn't a standard RIM/BIN produced by macro.
                // Just stop pass one, let pass two deal with it.
                DIAGNOSTIC(DIAG_ERR, "Unterminated RIM block at tape position %d, word 0%o\n",
                    tape_loc, word);
                return;
            }
            break;

        case LOADING:
            // curAddr will be the full 16 bit address for am1
            switch( (*loaderP)(fP, LOADER_CMD_NEXT, &curAddr, &word, NULL) )
            {
            case LOADER_DONE:
            case LOADER_STOP:
                state = DONE;
                break;

            case LOADER_AGAIN:
                continue;

            case LOADER_MORE:
                markValidByInstruction(FULLADDR(curAddr), word);
                break;

            case LOADER_ERROR:
                fprintf(stderr,"Loader returned an error at read location %d\n", tape_loc);
                fprintf(stderr,"Terminating.\n");
                fclose(fP);
                exit(1);
            }
            break;

        case DONE:
            // Add the start address
            markTarget(curAddr, 0, false);
            return;

        default:
            fprintf(stderr ,"Bad state %d\n", state);
            break;
        }
    }
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
            ch &= 077;
            if( ch == 0 )           // suppress multiple spaces
            {
                saw_space = 1;
            }
            else
            {
                if( !asMacro )
                {
                    if( verbose && showLeader && saw_space )
                    {
                        fprintf(outfP,"\n");
                        saw_space = 0;
                    }

                    if( verbose && showLeader )
                    {
                        printTapeLeader(ch);
                    }
                }
            }
        }
    }
    
    return( word );
}

// Read the next binary word, return it or -1 if EOF
int
nextWord(FILE *fP)
{
int word;
int count;
int ch;

    word = 0;
    count = 3;              // we need 3 tape bytes for a word

    while( count )
    {
        if( (ch = fgetc(fP)) == EOF )
        {
            return( -1 );           // sorry
        }

        ++tape_loc;

        if( !(ch & 0200) )
        {
            continue;               // skip non-binary char
        } 

        word = (word << 6) | (ch & 077);
        --count;
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
char tmpstr[128];

    // Assume it's a leader label as punched by macro
    for( i = 0; i < 8; i++ )
    {
        tmpstr[i] = (ch & 0200)?'*':' ';
        ch <<= 1;
    }
    
    tmpstr[8] = 0;
    fprintf(outfP,"%s\n",tmpstr);
}

// format an instruction into printed form
void
formatInstr(int pc, int word)
{
int tmp;
bool needBank = false;
char symbolstr[256];
char tmpstr[256];

    if( BANKNUM(pc) != curBank )
    {
        if( asMacro )
        {
            fprintf(outfP,"/ WARNING - this code uses extended memory, it will not work properly!\n");
        }

        needBank = true;
        curBank = BANKNUM(pc);
    }

    if( getLabel(FULLADDR(pc), pc, symbolstr) != -1 )
    {
        if( !verbose  )
        {
            strcat(symbolstr,",");
        }

        for( tmp = strlen(symbolstr) + 1; tmp++ < 6; )
        {
            strcat(symbolstr, " ");
        }
    }
    else
    {
        symbolstr[0] = '\0';
    }

    if( !verbose )
    {
        if( needBank )
        {
            fprintf(outfP,(asMacro)?"/ Now in bank %o\n":"bank %o\n", curBank);
        }

        if( pc > (lastAddr + 1) )
        {
            fprintf(outfP,"%o/\n", pc & 07777);    // addr is relative to current bank
        }

        lastAddr = pc;
        fprintf(outfP,"%s ", (symbolstr[0] != '\0')?symbolstr:"     ");
    }
    else
    {
        if( needBank )
        {
            fprintf(outfP,"%-5d %06o: bank %o\n", tape_loc, pc, curBank);
        }

        fprintf(outfP,"%-5d %06o: %06o %s ", tape_loc, pc, word, (symbolstr[0] != '\0')?symbolstr:"     ");
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

    decodeInstr(word, word & 07777, asMacro, separator, symbolstr, tmpstr, 0);
    fprintf(outfP,"%s\n", tmpstr);
}

// Set memory location as used, i.e. was loaded by loader
LabelP
markValid(int address, int iFlags)
{
LabelP labelP;

    address &= (BANKS * MEMSIZE) - 1 ; // for safety, limit to our supported size
    labelP = getLabelPointer(address, true);
    labelP->flags |= MEM_USED;
    labelP->instFlags = iFlags;

    return(labelP);
}

// Mark the address as used, and if word is an instruction that references memory, mark the target also
void
markValidByInstruction(int addr, int word)
{
int operand;
int iFlags;
LabelP labelP;
char dummy[128];

    // Unfortunately, we have to decode just for the flags
    decodeInstr(word,  addr, false, 0, 0, dummy, &iFlags);
    labelP = markValid(addr, iFlags);            //this address is used
    labelP->flags |= MEM_LOADED;
    operand = OPERAND(word);
    if( iFlags & (INSTR_READS | INSTR_WRITES) )
    {
        // Indirection isn't a write, it's a read
        markTarget(FULLADDR(operand), addr, (iFlags & (INSTR_WRITES | INSTR_INDIRECT)) == INSTR_WRITES);
    }
}

// Set memory location as used and the target of an instruction, e.g. JMP address.
// Record thd address that created this also.
void
markTarget(int address, int creator, bool modified)
{
LabelP labelP;

    labelP = getLabelPointer(address, true);
    labelP->refAddr = creator;
    labelP->flags |= MEM_TARGET | MEM_USED | (modified?MEM_MODIFIED:0);
}

// Return the label number if the address has been loaded and is the target of a memory reference, else -1
// Format the proper result, either a label or the address if no label defined.
int
getLabel(int address, int defaultval, char* rsltP)
{
LabelP labelP;

    address &= (BANKS * MEMSIZE) - 1 ; // for safety, limit to our supported size
    labelP = getLabelPointer(address, false);

    if( labelP && (labelP->flags & MEM_LOADED) && labelP->label[0] )
    {
        sprintf(rsltP,"%s", labelP->label);
        return( address );
    }
    else
    {
        sprintf(rsltP,"%04o", defaultval);      // no label assigned
        return( -1 );
    }
}

// Go through the label list and assign the visible names to used entries
// The label names will be generted from their use if it can be determined.
// The default name will be Lxxxx, which usually means it was the target of a jmp, jsp, or jda.
// Locations that are only read will be Cxxxx, for a constant.
// Locations that are written will be Vxxxx, for a variable.
void
generateLabels()
{
int address;
char *cP;
LabelP labelP;

    for( address = 0; address < (MEMSIZE * BANKS); ++address )
    {
        if( !(labelP = getLabelPointer(address, false)) )  // not a referenced address
        {
            continue;
        }

        // The label might have been preloaded from a symbol file, leave it if so
        if( (labelP->flags & MEM_TARGET) && !labelP->label[0] )
        {
            // construct the label
            cP = labelP->label;

            // Figure out if it's a variable, tranfer target, or data in that order
            // If a location is both a variable and a transfer target, the name will be VTxxxx.
            if( labelP->flags & MEM_MODIFIED )
            {
                sprintf(cP, "V%s%d", (labelP->instFlags & INSTR_VALID)?"T":"", var_number++);
            }
            else if( labelP->instFlags & INSTR_VALID )
            {
                sprintf(cP, "L%d", label_number++);
            }
            else
            {
                sprintf(cP, "C%d", const_number++);
            }
        }
    }
}

// Get the label pointer for the address, possibly creating it if necessary.
// Return the pointer.
LabelP
getLabelPointer(int address, bool create)
{
LabelP lblP;

    if( !(lblP = memlocs[address]) && create )
    {
        lblP = (LabelP)calloc(1, sizeof(Label));
        memlocs[address] = lblP;
    }

    return( lblP );
}

// See if there are any used memory locations that were not loaded.
// If so, they were macro1 variables, emit them.
void
checkForVariables(FILE *outfP, int addr, int endAddr)
{
LabelP labelP;

    while( ++addr < endAddr )
    {
        if( (labelP = getLabelPointer(addr, false)) )
        {
            if( !(labelP->flags & (MEM_LOADED | MEM_EMITTED)) && labelP->label[0] )
            {
                labelP->flags |= MEM_EMITTED;
                fprintf(outfP, "%s, 0\n", labelP->label);
            }
        }
    }
}

// Look for a loader by name.
// If found, return its pointer, else null.
LoaderP
findLoader(LoaderMap loaders[], char *nameP)
{
int i;
LoaderMapP mapP;

    for( i = 0; (mapP = &loaders[i++]) && mapP->nameP; )
    {
        if( !strcmp(mapP->nameP, nameP) )
        {
            return( mapP->loaderP );
        }
    }

    return(0);
}

// Parse a loader name and optional args of the form name[,arg]...
// Optional arg pointers are placed in args, terminated by a null entry.
// An empty paramter, ",,", will place a pointer to an empty string, "" in the entry.
// Return the loader name if at least a loader name was found, else null.
char *
parseLoader(char *strP, char *args[])
{
int i;
char *nameP;
char *cP;

    if( !(nameP = strsep(&strP, ",")) )
    {
        return(nameP);          // no loader name, error
    }

    DIAGNOSTIC(DIAG_LDR, "Loader name is '%s'", nameP);

    for( i = 0; (cP = strsep(&strP, ",")); )
    {
        DIAGNOSTIC(DIAG_LDR, "Loader param %d is '%s'", i, cP);
        args[i++] = cP;
    }

    args[i] = NULL;
    return( nameP );
}

void
showLoaders()
{
int i;
LoaderMapP mapP;

    printf("The following loaders are supported:\n");

    for( i = 0; (mapP = &loaders[i++]) && mapP->nameP; )
    {
        printf("%s\t%s\n", mapP->nameP, mapP->descriptionP);
    }
}

// Try to load symbol definitions from an am1 format symbol table file.
// Any lines that don't begin with a digit are ignored.
// If successful, return true, else false.
bool
loadSymbols(char *filenameP)
{
int addr;
int lineNo;
char *cP, *cP2;
LabelP labelP;
FILE *fP;
char line[256];

    if( !(fP = fopen(filenameP,"r")) )
    {
        return( false );
    }

    lineNo = 3;

    while( fgets(line, sizeof(line), fP) )
    {
        ++lineNo;

        if( !isdigit(line[0]) )
        {
            continue;
        }

        if( line[0] == '#' )
        {
            continue;       // not currently used, but might be a comment eventually
        }

        line[strlen(line) - 1] = '\0';   // drop newline

        addr = strtol(line, &cP, 8);    // symbol addrs are always octal
        // we also ignore the symbol type character, as long as there is some alpha char
        if( (*cP++ != ' ') || !isalpha(*cP++) || (*cP++ != ' ') )
        {
            // Not a valid definition line
            fprintf(stderr, "Invalid line %d in symbol file.\n", lineNo);
            fclose(fP);
            return( false );
        }

        // cP now points to the symbol name, terminate it
        cP2 = cP;
        while( !isspace(*cP2) )
        {
            ++cP2;
        }

        *cP2 = '\0';
        cP[LABELSIZE-1] = '\0';  // limit the length

        labelP = getLabelPointer(addr & 0xFFFF, true);  // constrain the address just in case
        strcpy(labelP->label, cP);
    }

    fclose(fP);
    return(true);
}

// Set the bit in the memory map corresponding to the address passed.
// If it was not already set, return true, else if already set, false.
bool
setBit(uint64_t map[], int addr)
{
int idx;
uint64_t bit;

    idx = addr >> 6;            // each map entry is 64 memory locations
    bit = UINT64_C(1) << (addr & 63);

    if( map[idx] & bit )
    {
        return( false );
    }

    map[idx] |= bit;
    return(true);
}

// See if the bit in the memory map corresponding to the address passed is set.
// If so, return true, else false.
bool
isBitSet(uint64_t map[], int addr)
{
int idx;
uint64_t bit;

    idx = addr >> 6;            // each map entry is 64 memory locations
    bit = UINT64_C(1) << (addr & 63);

    return( (map[idx] & bit)?true:false );
}

void
usage()
{
    fprintf(stderr,"Usage: disassemble -[?[a|m|v]klnr] [-L loader[,arg...]] [-s symfile] [-o outfile] infile\n");
    fprintf(stderr,"where:\n");
    fprintf(stderr,"? - list the supported loaders then exit\n");
    fprintf(stderr,"a - output in am1 assembler form\n");
    fprintf(stderr,"m - output in macro assember form, warn about extended memory use\n");
    fprintf(stderr,"v - verbose mode, output in listing-style\n");
    fprintf(stderr,"k - keep RIM loader code if seen and in macro mode; normally no because MACRO usually adds it\n");
    fprintf(stderr,"l - output the leader in readable form, only in verbose\n");
    fprintf(stderr,"n - don't output a location if it has already been output\n");
    fprintf(stderr,"s name - symbol definition file to use\n");
    fprintf(stderr,"o name - output file name, defaults to stdout\n");
    fprintf(stderr,"r - raw mode, just dump every binary word as an instruction, no RIM or BIN checking\n");
    fprintf(stderr,"    raw overrides all other flags except d\n");
    fprintf(stderr,"L loader - use the named loader with optional loader arguments\n");
    fprintf(stderr,"Flags can be together, -mid, or separate, -m -i -d\n");
    fprintf(stderr,"Flags that take an argument can be of the form e.g. -L xxx or -Lxxx\n");
    exit(1);
}
