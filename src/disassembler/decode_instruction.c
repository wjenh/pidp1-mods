/*
 * Decode a PDP-1 instruction into a readable form.
 * This is intended to provide an embeddable instrcution formatter for use by other tools to
 * produce a decoded instruction suitable for display toa user.
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
 * 27/10/2025 wje - Initial version
 *
 */
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

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
    unsigned int value;     // the value of the operand field to match
    char *name;             // instruction name to output
    SpecialMods modifiers;  // eg, if the instruction can wait for I/O completion
    int mask;               // the bits in the operand to use for comparison to value, value == (oprerand & mask)
} Special;

Special *findSpecial(Special *, int);

// Opcodes use only the high 5 bits, not the indirect bit, which means we only need 32 opcode entries.
// This generally works, except for the JDA instruction, 17, which is handled specially.

static CodeDef opcodes[] =                 // we don't use the indirect bit, so we have only 32 possibilities
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
static Special iots[] =
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
static Special skips[] =
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
static Special shifts[] =
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

// Format an instruction into printed form, placing the results in the passed string.
// A pointer to the terminating null byte in the string is returned.
char *
decodeInstr(int word, char*resultP)
{
unsigned int opcode;
unsigned int operand;
int indirect;
int tmp, tmp2;
int bits03;
char *cP;
char tmpstr[32];
char tmpstr2[32];
CodeDef *instructionP;
Special *sP;

    opcode = OPERATION(word);
    if( opcode > 077 )
    {
        resultP += sprintf(resultP,"Invalid word, greater than 0777777\n");
        return(resultP);
    }

    indirect = opcode & 01;
    opcode >>= 1;                                           // convert to 32 possible instructions

    operand = OPERAND(word);
    instructionP = &opcodes[opcode];

    if( instructionP->modifiers == IS_ILLEGAL )            // not an instruction, just emit the octal value
    {
        resultP += sprintf(resultP,"%06o", word);
        return(resultP);
    }

    switch( instructionP->modifiers )
    {
    case NONE:
        resultP += sprintf(resultP,"%s", instructionP->name);
        break;

    case CAN_INDIRECT:
        resultP += sprintf(resultP,"%s%s %06o", instructionP->name, (indirect)?" i":"", operand);
        break;

    case IS_CALJDA:
        if( indirect )
        {
            resultP += sprintf(resultP,"jda %06o", operand);
        }
        else
        {
            resultP += sprintf(resultP,"%s", instructionP->name);            // CAL
        }
        break;

    case IS_LAW:
        // The versions of macro1 floating arund are broken, they don't hande the 'law -n' syntax properly.
        //resultP += sprintf(resultP," %s %s%04o", instructionP->name, (indirect)?"-":"", operand);
        resultP += sprintf(resultP,"%s %s%04o", instructionP->name, (indirect)?"i ":"", operand);
        resultP += sprintf(resultP," (%d dec)", (indirect)?-operand:operand);
        break;

    case IS_SHIFT:
        tmp = ((indirect?010000:0) | operand) & 017000;      // indirect bit controls left-right
        sP = findSpecial(shifts, tmp);
        if( sP->modifiers == UNKNOWN )
        {
            resultP += sprintf(resultP,"%06o", word);       // a bare shift doesn't exist, must be data
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

            resultP += sprintf(resultP,"%s", sP->name);
            if( tmp2 != 0 )
            {
                resultP += sprintf(resultP," %d (decimal)", tmp2);        // only if we'e actually shifting
            }
        }
        break;

    case IS_SKIP:
        sP = findSpecial(skips, operand);
        resultP += sprintf(resultP,"%s", sP->name);
        if( indirect )
        {
            resultP += sprintf(resultP," not");
        }

        switch( sP->modifiers )
        {
        case UNKNOWN:
            resultP += sprintf(resultP," %04o", operand);
            break;

        case IS_SZF:
            resultP += sprintf(resultP," %0o", (operand & 07));
            break;

        case IS_SZS:
            resultP += sprintf(resultP," %0o", ((operand >> 3) & 07));
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
                sprintf(tmpstr2,"%slat", tmp?"|":"");
                strcat(tmpstr, tmpstr2);
                tmp = 1;
            }
            if( operand & OPR_MASK_CLA )        // MUST come after LAT!
            {
                operand &= ~OPR_MASK_CLA;
                sprintf(tmpstr2,"%scla", tmp?"|":"");
                strcat(tmpstr, tmpstr2);
                tmp = 1;
            }
            if( operand & OPR_MASK_CLI )
            {
                operand &= ~OPR_MASK_CLI;
                sprintf(tmpstr2,"%scli", tmp?"|":"");
                strcat(tmpstr, tmpstr2);
                tmp = 1;
            }
            if( operand & OPR_MASK_CMA )
            {
                operand &= ~OPR_MASK_CMA;
                sprintf(tmpstr2,"%scma", tmp?"|":"");
                strcat(tmpstr, tmpstr2);
                tmp = 1;
            }
            if( operand & OPR_MASK_HLT )
            {
                operand &= ~OPR_MASK_HLT;
                sprintf(tmpstr2,"%shlt", tmp?"|":"");
                strcat(tmpstr, tmpstr2);
                tmp = 1;
            }
            if( !(operand & OPR_MASK_STF) && bits03 )
            {
                // CLF is bits03, already cleared
                sprintf(tmpstr2,"%sclf %o", tmp?"|":"", bits03);
                strcat(tmpstr, tmpstr2);
                bits03 = 0;                  // done with these
                tmp = 1;
            }
            if( (operand & OPR_MASK_STF) && bits03 )
            {
                operand &= ~OPR_MASK_STF;
                sprintf(tmpstr2,"%sstf %o", tmp?"|":"", bits03);
                strcat(tmpstr, tmpstr2);
                bits03 = 0;                  // done with these
                tmp = 1;
            }
        }

        if( (operand | bits03) != 0 )                      // extra bits were left over, must be data
        {
            resultP += sprintf(resultP,"%06o", word);
        }
        else
        {
            resultP += sprintf(resultP,"%s", tmpstr);
        }
        break;

    case IS_IOT:
        // Sometimes there are extra bits in the operand above the usual 6 bits
        tmp2 = operand & 037700;

        sP = findSpecial(iots, operand);
        resultP += sprintf(resultP,"%s", sP->name);

        switch( sP->modifiers )
        {
        case UNKNOWN:
            if( operand != 0 )                  // not an empty IOT
            {
                if( indirect )
                {
                    operand |= 010000;       // include the bit in the output
                }

                resultP += sprintf(resultP,"|%05o", operand);
            }
            break;

        case CAN_WAIT:
            if( indirect )
            {
                resultP += sprintf(resultP," w");
            }
            break;

        case INVERT_WAIT:       // because macro automatically sets, xx-i means not set
            if( !indirect )
            {
                resultP += sprintf(resultP,"-i");
            }
            break;

        case IS_DPY:            // does INVERT_WAIT plus intensity
            if( !indirect )
            {
                resultP += sprintf(resultP,"-i");
            }

            tmp = operand & 0700;  // macro doesn't print the intensity if it's zero 
            if( tmp > 0 )
            {
                resultP += sprintf(resultP," %3o", tmp);
            }

            tmp2 = 0;              // dpy uses some of the special bits
            break;

        case HAS_ID:            // some encode a subdevice, eg tape drive number
            resultP += sprintf(resultP," %02o", (operand >> 6) & 077);
            tmp2 = 0;
            break;
        }

        if( (sP->modifiers != UNKNOWN) && (tmp2 != 0) )
        {
            resultP += sprintf(resultP," | %04o", tmp2);   // add extra bits
        }
        break;
    }

    return( resultP );
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
