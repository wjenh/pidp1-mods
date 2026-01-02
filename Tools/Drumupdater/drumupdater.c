/*
 * Read a rim and/or bin format tape, copy it to the Type 23 Parallel drum image.
 *
 * PDP-1 binary tapes are loaded initially using read-in, which ignores any character that doesn't have
 * bit 0200 set.
 * However, many tapes have a leader punched that shows a punch pattern that makes descriptive text.
 * This is dropped.
 *
 * Processing then continues looking for a RIM block, followed by ddt-form BIN blocks.
 * 
 * The initial loader is discarded and the actual program words are copied to the given drum
 * track.
 * Only locations 0-07750 are copied because 07751 and up are where the loader usually resides,
 * not needed for the drum image.
 * Instead, that space is used for storing the program's start address and name.
 *
 * See disassemble_tape.c for details on the loaders.
 *
 * Usage: drumupdater [-i imagefile] [-l label] [-a] -t trackno filename
 * where:
 * i - use the given name for the drum image instead of 'drumImage'
 * l - label the track with the given label, up to 12 flexo characters
 * a - read the tape image as an AM1 loader format tape
 * t - the track to store to, 0-31 dec.
 *
 * Original author: Bill Ezell (wje), pdp1@quackers.net
 *
 * Revision history:
 *
 * 28/11/2025 wje - Initial version
 * 02/01/2026 wje - Add AM1 loader format support, update the usage documentation
 *
 */
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdbool.h>
#include <string.h>

#define DEFAULT_IMAGE "drumImage"

// Instructions have a 5 bit opcode followed by a 1 bit indirect marker as the high 6 bits of a word
#define OPERATION(x)    (x >> 12)

// The remaining 12 low bits are the operand whose meaning varies by instruction
#define OPERAND(x)      (x & 0007777)

// The processing loop is a simple state machine
typedef enum {START, RESTART, LOOKING, RIM, BIN, DATA, RAW, DONE} State;
typedef int Word;        // we put the 18 bit PDP-1 words into a 32 bit integer

Word getWord(FILE *, int);
void usage(void);

int tape_loc;
State state;

// This array is used to hold the tape image for writing to the drum.
Word memlocs[4096];     // enough for the entire address space

int
main(int argc, char **argv)
{
int i, j;
int opt;
int trackNo = 0;
int cur_addr;
int end_addr;               // for BIN loader
int start_addr = 4;         // for macro start, can come from RIM if no BIN blocks, 4 is the default if none
bool am1Mode = false;
char *filenameP;
char *cP, *cP2;
char imageName[512];
char label[128];
Word word;

bool did_start = false;
FILE *fP;
int outfd;

    label[0] = 0;
    strcpy(imageName, DEFAULT_IMAGE);

    // parse our comd line args
    while( (opt = getopt(argc, argv, "i:l:t:a")) != -1 )
    {
        switch( opt )
        {
        case 'i':
           strcpy(imageName, optarg);
           break;

        case 't':
           trackNo = strtol(optarg, &cP, 0);
           if( (cP == optarg) || *cP )
           {
               trackNo = -1;               // invalid number
           }
           break;

        case 'l':
           strcpy(label, optarg);
           break;

        case 'a':
           am1Mode = true;
           break;

        default: /* '?' */
	   usage();
        }
    }

    if( optind >= argc )
    {
        usage();                // should only be the input filename
    }

    if( (trackNo > 31 ) || (trackNo < 0) )
    {
        fprintf(stderr,"The track number must be 0-31\n");
        exit(1);
    }

    filenameP = argv[optind];

    if( !(fP = fopen(filenameP, "r")) )
    {
        fprintf(stderr,"Can't open file '%s'\n", filenameP);
        exit(1);
    }

    if( (outfd = open(imageName, O_CREAT + O_WRONLY, 0666)) < 0 )
    {
        fprintf(stderr,"Can't open drum image file '%s'\n", imageName);
        exit(1);
    }

    // The default label is the filename without any leading path or trailing extension.
    if( !label[0] )
    {
        if( (cP2 = strrchr(filenameP, '/')) )
        {
            strcpy(label, ++cP2);
        }
        else
        {
            strcpy(label, filenameP);
        }

        if( (cP2 = strrchr(label, '.')) )
        {
            *cP2 = 0;
        }
    }

    state = START;

    // The state machine loop
    for(;;)
    {
        if( state == DONE )
        {
            break;
        }

        word = getWord(fP, state);

        if( word == -1 )
        {
            break;          // all done
        }

        switch( state )
        {
        case START:
            if( OPERATION(word) == 032 )            // beginning of the RIM code block
            {
                state = RIM;
                cur_addr = OPERAND(word);
                word = getWord(fP, state);
                memlocs[cur_addr] = word;
            }
            break;

        case RIM:
            if( OPERATION(word) == 060 )        // end of RIM code block
            {
                start_addr = OPERAND(word);     // in case no BIN block follows
                state = LOOKING;                // look for a BIN block now
            }
            else if( OPERATION(word) == 032 )   // next data word to load
            {
                cur_addr = OPERAND(word);
                word = getWord(fP, state);
                memlocs[cur_addr] = word;
            }
            else
            {
                // We didn't get what we expected, not a standard macro-generated tape.
                fprintf(stderr,"Nonstandard binary tape, unterminated RIM block at tape location %d\n",
                    tape_loc - 3);
                fprintf(stderr,"Terminating.\n");
                fclose(fP);
                exit(1);
            }
            break;

        case LOOKING:

            if( am1Mode )
            {
                if( word & 0600000 )
                {
                    // am1 loader end-of-code, start addr
                    start_addr = word & 0177777;    // maybe it will support other than bank 0 eventually
                    state = DONE;
                }
                else
                {
                    if( word > 07777 )
                    {
                        fprintf(stderr,"AM1 binary uses a bank other than 0, can't load to drum.\n");
                        fclose(fP);
                        exit(1);
                    }
                    cur_addr = word;        // start of am1 block
                    end_addr = getWord(fP, state);
                    state = BIN;
                }
            }
            else if( OPERATION(word) == 032 )   // RIM ended, DIO, beginning of BIN block
            {
                cur_addr = OPERAND(word);             // starting address
                word = getWord(fP, state);            // shold be 'dio endaddr + 1'
                if( OPERATION(word) != 032 )          // not, so this is not in ddt bin format
                {
                    fprintf(stderr,
                "Looking for a BIN block, but saw a non-standard block at tape location %d, terminating.\n",
                        tape_loc = 3);
                    fclose(fP);
                    exit(1);
                }

                end_addr = OPERAND(word);       // last location + 1, in checksum as the dio instruction
                state = BIN;
            }
            else if( OPERATION(word) == 060 )   // JMP, done loading BIN blocks, end of valid input
            {
                state = DONE;
                start_addr = OPERAND(word);
                memlocs[cur_addr] = word;        // keep the JMP
            }
            else
            {
                // Random data outside a RIM or BIN
                fprintf(stderr, "Extra data after end of program, ignored.\n");
                state = DONE;
            }
            break;

        case BIN:
            {
                // word will contain the 18 bit value for the current pc
                memlocs[cur_addr++] = word;

                if( cur_addr >= end_addr )      // done
                {
                    if( !am1Mode )
                    {
                        word = getWord(fP, state);         // checksum, ignore
                    }
                    state = LOOKING;
                }
            }
            break;

        default:
            printf("Bad state %d\n", state);
            break;
        }
    }

    // memlocs is complete, start_addr is the starting address from the tape
    // The PDP-1 drumloader, etc. expect the start address in location 07773, add it
    // Location 07774 will be 1 to indicate the presence of a label.
    // We also put an 'initialized' marker of 0707070 at locations O7775, 07776, and 07777
    memlocs[07773] = start_addr;
    memlocs[07775] = 0707070;
    memlocs[07776] = 0707070;
    memlocs[07777] = 0707070;

    // Locations 07751-07771 will have the label, up to 34 characters.
    // Characters are packed 2 per word, first character in the high 9 bits, second in the low 9 bits, etc.
    // A character of 0, a null byte, marks the end of the label.
    label[34] = 0;      // be sure not too long
    j = strlen(label);
    cur_addr = 07751;

    if( j == 0 )
    {
        memlocs[07751] = 0;             // no label, zero first location just in case it was set during loading
    }
    else
    {
        for( i = 0; i < j; ++i )
        {
            if( i & 1 )         // odd char, low byte
            {
                memlocs[cur_addr++] |= label[i];
            }
            else
            {
                memlocs[cur_addr] = label[i] << 9;
            }
        }
        
        if( !(i & 1) )
        {
            memlocs[cur_addr] = 0;           // be sure it's terminated
        }
    }

    memlocs[07774] = (j > 0)?1:0;       // mark if we had a label or not

    // write the memory image to the drum image
    lseek(outfd, trackNo * sizeof(memlocs), SEEK_SET);
    write(outfd, memlocs, sizeof(memlocs));
    fclose( fP );
    close(outfd);

    printf("Tape loaded to track %d with label '%s', start address is %o\n",
        trackNo, label, start_addr);

    exit(0);
}

// Return the next 18 bit word, 3 tape characters, from the 'tape', or -1 if EOF.
// If a word was pushed back, return it instead.
// Since this is the equivalent of the RPB instruction, ignore any without bit o200 set.

int
getWord(FILE *fP, int state)
{
int word;
int count;
int ch;
int last_ch;

    word = 0;
    count = 3;              // we need 3 tape bytes for a word
    ch = last_ch = 0;       // character read before the current character, needed for STOP detection

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
    }
    
    return( word );
}

void
usage()
{
    fprintf(stderr,"Usage: drumupdater [-i imagefile] [-l label] [-a] -t trackno filename\n");
    fprintf(stderr,"where:\n");
    fprintf(stderr,"i - use the given name for the drum image instead of 'drumImage'\n");
    fprintf(stderr,"l - label the track with the given label, up to 12 flexo characters\n");
    fprintf(stderr,"a - read the tape image as an AM1 loader format tape\n");
    fprintf(stderr,"t - the drum track to load, 0-31\n");
    exit(1);
}
