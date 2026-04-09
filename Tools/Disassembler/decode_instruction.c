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
 * 09/02/2026 wje - Fix completion bit handling, dpy i and c handling
 * 01/03/2026 wje - Fix cal, emit operand if it actually isn't a cal
 * 15/03/2026 wje - Add more 1D instructions, improve extra bits reporting
 * 19/03/2026 wje - Add even more 1D instructions, fix macro mode for them
 * 21/03/2026 wje - Print i and C for unknown IOTs
 * 29/03/2026 wje - Exclude symbolic name for iots and 1D instructions not supported by macro if in macro mode
 * 31/03/2026 wje - Add instruction type flags
 *
 */
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

// For the type flags
#include "decode_instruction.h"

// Instructions have a 5 bit opcode followed by a 1 bit indirect marker as the high 6 bits of a word
// IOTs can also have a completion-requested, bit 6 set, 04000.
#define OPERATION(x)    (((x) >> 13) & 037)

// The remaining 13 low bits are the operand whose meaning varies by instruction
// The indirect bit is included because some instructions attach a different meaning to the bit.
#define OPERAND(x)      (x & 0017777)

#define INDIRECT_BIT 010000
#define COMPLETION_BIT 04000

// The operate instruction is a pain, has to be done bitwise in ad-hoc code
#define OPR_MASK_CLA 00200
#define OPR_MASK_CLF 00007
#define OPR_MASK_CLI 04000
#define OPR_MASK_CMA 01000
#define OPR_MASK_HLT 00400
#define OPR_MASK_LAT 02200
#define OPR_MASK_NOP 017777
#define OPR_MASK_LAI 00040
#define OPR_MASK_LIA 00020
#define OPR_MASK_STF 00010  // and CLF is not CLF with nonzero low 3 bits
#define OPR_MASK_CMI 010000

// Same for the 1D extended operations
#define OPR2_MASK_SCF 00040
#define OPR2_MASK_SCI 00100
#define OPR2_MASK_IFI 02000
#define OPR2_MASK_IIF 04000
#define OPR2_MASK_IDA 00400

// And the skips
// 1D skips, SZI is sni -i, but there was the sni opcode for it
#define SKP_MASK_SZI 014000
#define SKP_MASK_SNI 04000
// Regular skips
// SZF and SZS are even more special, 
#define SKP_MASK_SMA 00400
#define SKP_MASK_SPA 00200
#define SKP_MASK_SPI 02000
#define SKP_MASK_SZA 00100
#define SKP_MASK_SZO 01000
#define SKP_MASK_SZF 00007
#define SKP_MASK_SZS 00070

// IOT and other instructions have a number of special behaviors
typedef enum {UNKNOWN, NORMAL, CAN_WAIT, INVERT_WAIT, IS_DPY, IS_SZF, IS_SZS, IS_SZI, HAS_ID} SpecialMods;

// This is used to handle instructions that aren't standard, such as the IOT, OPR, etc. instructions
typedef struct
{
    unsigned int value;     // the value of the operand field to match
    char *name;             // instruction name to output
    SpecialMods modifiers;  // eg, if the instruction can wait for I/O completion
    int mask;               // the bits in the operand to use for comparison to value, value == (oprerand & mask)
    bool notMacro;          // if true, not supported by macro
} Special;

// Opcodes use only the high 5 bits, not the indirect bit, which means we only need 32 opcode entries.
// This generally works, except for the JDA instruction, 17, which is handled specially.

static CodeDef opcodes[] =                 // we don't use the indirect bit, so we have only 32 possibilities
    {
        { "illegal", IS_ILLEGAL, INSTR_NOTONE },        // OP 0
        { "and", CAN_INDIRECT, INSTR_READS},            // OP 2
        { "ior", CAN_INDIRECT, INSTR_READS},            // OP 4
        { "xor", CAN_INDIRECT, INSTR_READS},            // OP 6
        { "xct", CAN_INDIRECT, INSTR_XCT|INSTR_READS},  // OP 10
        { "illegal", IS_ILLEGAL, INSTR_NOTONE},         // OP 12
        { "illegal", IS_ILLEGAL, INSTR_NOTONE},         // OP 14
        { "cal", IS_CALJDA, INSTR_JUMPS|INSTR_WRITES},  // OP 16, special, could be JDA
        { "lac", CAN_INDIRECT, INSTR_READS},            // OP 20
        { "lio", CAN_INDIRECT, INSTR_READS},            // OP 22
        { "dac", CAN_INDIRECT, INSTR_WRITES},           // OP 24
        { "dap", CAN_INDIRECT, INSTR_WRITES},           // OP 26
        { "dip", CAN_INDIRECT, INSTR_WRITES},           // OP 30
        { "dio", CAN_INDIRECT, INSTR_WRITES},           // OP 32
        { "dzm", CAN_INDIRECT, INSTR_WRITES},           // OP 34
        { "illegal", IS_ILLEGAL, INSTR_NOTONE},         // OP 36
        { "add", CAN_INDIRECT, INSTR_READS},            // OP 40
        { "sub", CAN_INDIRECT, INSTR_READS},            // OP 42
        { "idx", CAN_INDIRECT, INSTR_WRITES},           // OP 44
        { "isp", CAN_INDIRECT, INSTR_WRITES},           // OP 46
        { "sad", CAN_INDIRECT, INSTR_SKIPS|INSTR_READS},    // OP 50
        { "sas", CAN_INDIRECT, INSTR_SKIPS|INSTR_READS},    // OP 52
        { "mul", CAN_INDIRECT, INSTR_VALID},           // OP 54
        { "div", CAN_INDIRECT, INSTR_VALID},           // OP 56
        { "jmp", CAN_INDIRECT, INSTR_JUMPS|INSTR_READS},    // OP 60
        { "jsp", CAN_INDIRECT, INSTR_JUMPS|INSTR_WRITES},   // OP 62
        { "skp", IS_SKIP, INSTR_SKIPS},                // OP 64
        { "sft", IS_SHIFT, INSTR_VALID},               // OP 66
        { "law", IS_LAW, INSTR_VALID},                 // OP 70
        { "iot", IS_IOT, INSTR_VALID},                 // OP 72
        { "opr1D", IS_OPR2, INSTR_VALID},              // OP 73, extended 1D instruction
        { "opr", IS_OPR, INSTR_VALID}                  // OP 76
    };

// IOT decoding.
// A match is found if the word matches the first field exactly when masked by the last.
// The more specific masks for the same IOT should come first
static Special iots[] =
    {
        {010000, "ioh", IS_IOH, 017777, false},

        // BBN CLOCK
        {02032, "cks", NORMAL, 07777, true},
        {02132, "cct", CAN_WAIT, 07777, true},
        {00032, "rck", NORMAL, 07777, true},

        // Type 23 drum
        {02061, "dba", NORMAL, 07777, true},
        {00061, "dia", NORMAL, 07777, true},
        {02062, "dra", NORMAL, 07777, true},
        {00062, "dwc", NORMAL, 07777, true},
        {02063, "dss", CAN_WAIT, 07777, true},
        {00063, "dcl", CAN_WAIT, 07777, true},

        // SBS
        {00052, "isb", NORMAL, 07777, true},
        {00054, "lsm", NORMAL, 07777, false},
        {00055, "esm", NORMAL, 07777, false},
        {00056, "cbs", NORMAL, 07777, false},

        {04074, "eem", NORMAL, 07777, false},
        {00074, "lem", NORMAL, 07777, false},
        {00001, "rpa", INVERT_WAIT, 0777, false},
        {00002, "rpb", INVERT_WAIT, 0777, false},
        {00033, "cks", NORMAL, 07777, false},
        {00007, "dpy", IS_DPY, 077, false},
        {00005, "ppa", INVERT_WAIT, 077, false},
        {00006, "ppb", INVERT_WAIT, 077, false},
        {00030, "rrb", NORMAL, 077, false},
        {00004, "tyi", CAN_WAIT, 077, false},
        {00003, "tyo", INVERT_WAIT, 077, false},

        {00000, "iot", UNKNOWN, 0, false}       // special end marker if nothing matches, must be last
    };

// Shift/rotate decoding.
static Special shifts[] =
    {
        {001000, "ral", NORMAL, 017000, false},
        {011000, "rar", NORMAL, 017000, false},
        {003000, "rcl", NORMAL, 017000, false},
        {013000, "rcr", NORMAL, 017000, false},
        {002000, "ril", NORMAL, 017000, false},
        {012000, "rir", NORMAL, 017000, false},
        {005000, "sal", NORMAL, 017000, false},
        {015000, "sar", NORMAL, 017000, false},
        {007000, "scl", NORMAL, 017000, false},
        {017000, "scr", NORMAL, 017000, false},
        {006000, "sil", NORMAL, 017000, false},
        {016000, "sir", NORMAL, 017000, false},
        {00000, "sft", UNKNOWN, 0, true}      // special end marker if nothing matches, must be last
    };

static Special *findSpecial(Special *specP, int op);

// Format an instruction into printed form, placing the results in the passed string.
// If the instruction has an address field and the value matches addr,
// use the string symbolP if not null instead of the address.
// If flagsP is not null, return the INSTR_x flags in it.
// A pointer to the terminating null byte in the result string is returned.
// If defP is not null, a copy of the CodeDef entry for the opcode will be copied to it.
char *
decodeInstr(int word, int addr, bool asMacro, char *separatorP, char *symbolP, char *resultP, CodeDefP defP)
{
unsigned int opcode;
unsigned int operand;
int indirect;
int completion;
int tmp, tmp2;
int extra;
int bits03;
int flags;
char *cP;
char *operandStrP;
char tmpstr[32];
char tmpstr2[32];
char addrStr[32];
CodeDef *instructionP;
Special *sP;

    opcode = OPERATION(word);
    if( opcode > 077 )
    {
        resultP += sprintf(resultP, "%06o", word);
        if( defP )
        {
            memcpy(defP, &opcodes[0], sizeof(CodeDef));
        }

        return( resultP );
    }

    indirect = word & INDIRECT_BIT;
    completion = word & COMPLETION_BIT;
    operand = OPERAND(word);
    extra = 0;

    instructionP = &opcodes[opcode];

    // Copy to the passed ptr
    if( defP )
    {
        memcpy(defP, instructionP, sizeof(CodeDef));
    }

    flags = instructionP-> flags;           // initial flags, can be modified
    if( indirect && (instructionP->modifiers == CAN_INDIRECT) )
    {
        flags |= INSTR_INDIRECT;
    }

    if( flags & INSTR_NOTONE )
    {
        resultP += sprintf(resultP,"%06o", word);       // this one is easy

        if( defP )
        {
            defP->flags = flags;
        }
        return(resultP);
    }

    // Just in case the operand is an addr field and matches the passed addr.
    if( ((operand & 07777) == addr) && symbolP && *symbolP )
    {
        operandStrP = symbolP;
    }
    else
    {
        sprintf(addrStr, "%04o", (operand & 07777));
        operandStrP = addrStr;
    }

    switch( instructionP->modifiers )
    {
    case NO_MODS:
        resultP += sprintf(resultP,"%s", instructionP->name);
        break;

    case CAN_INDIRECT:
        resultP += sprintf(resultP,"%s%s %s", instructionP->name, (indirect)?" i":"", operandStrP);
        extra &= ~INDIRECT_BIT;
        break;

    case IS_CALJDA:
        // jda was a hack apparently, it is cal with the indirect bit set.
        if( indirect )
        {
            resultP += sprintf(resultP,"jda %s", operandStrP);
            indirect = 0;
            extra &= ~INDIRECT_BIT;
            flags &= ~INSTR_INDIRECT;
            if( defP )
            {
                defP->flags &= ~INSTR_INDIRECT;
            }

            flags |= INSTR_JDA;
        }
        else
        {
            if( operand )
            {
                // looks like cal, but has more bits, probably data
                resultP += sprintf(resultP, "%s %o", instructionP->name, operand);
            }
            else
            {
                resultP += sprintf(resultP, "%s", instructionP->name);            // CAL
            }
        }
        break;

    case IS_LAW:
        // The versions of macro1 floating arund are broken, they don't hande the 'law -n' syntax properly.
        resultP += sprintf(resultP,"%s %s%s", instructionP->name, (indirect)?"i ":"", operandStrP);
        if( !asMacro )
        {
            resultP += sprintf(resultP," (%d dec)", (indirect)?-operand:operand);
        }
        break;

    case IS_SHIFT:
        tmp = operand & 017000;      // indirect bit controls left-right
        sP = findSpecial(shifts, tmp);
        if( sP->modifiers == UNKNOWN )
        {
            resultP += sprintf(resultP,"%06o", word);       // a bare shift doesn't exist, must be data
            flags = INSTR_NOTONE;
        }
        else if( sP->notMacro && asMacro )
        {
            resultP += sprintf(resultP,"%06o", word);
        }
        else
        {
            // Shift/rotate is strange. The number of places to shift/rotate is the count of 1's in the lower 9 bits
            // This one is problematic. Technically any 1 bits in any positon count, but sometimes
            // data words are like that.
            // Macro uses consecutive bits, we'll go with that and if not, then it's not a shift/rotate.
            tmp = operand & 0777;
            tmp2 = 0;
            while( tmp & 01 )
            {
                ++tmp2;
                tmp >>= 1;
            }

            if( tmp )
            {
                // more bits left, fail
                tmp2 = -1;
            }

            if( tmp2 < 0 )
            {
                resultP += sprintf(resultP,"%06o", word);
                flags = INSTR_NOTONE;
            }
            else
            {
                resultP += sprintf(resultP,"%s", sP->name);
                if( tmp2 != 0 )
                {
                    // only if we'e actually shifting
                    resultP += sprintf(resultP,asMacro?" %ds":" %d (decimal)", tmp2);
                }
            }
        }
        break;

    case IS_SKIP:
        // This one is a pain because multiple skips can be combined.
        // Macro1 allows an 'or' operation, 'a!b', which is used in macro mode.
        // If we see any extra bits not valid microinstuctions, then just the octal data is output.
        // The first 2 are 1D instructions
        tmp = 0;                // set to 1 if we have printed one already
        tmpstr[0] = 0;          // be sure we start empty

        if( indirect )
        {
            operand &= ~INDIRECT_BIT;
        }

        if( (operand & SKP_MASK_SZI) == SKP_MASK_SZI )    // MUST come befoe SNI! Only 2 bit directive.
        {
            if( asMacro )       // not in macro
            {
                sprintf(tmpstr,"%06o", word);
                operand = 0;
            }
            else
            {
                sprintf(tmpstr,"szi");
                tmp = 1;
                operand &= ~SKP_MASK_SZI;
                indirect = 0;
            }
        }

        if( (operand & SKP_MASK_SNI) == SKP_MASK_SNI )
        {
            if( asMacro )       // not in macro
            {
                sprintf(tmpstr,"%06o", word);
                operand = 0;
            }
            else
            {
                sprintf(tmpstr2,"%ssni", tmp?((asMacro)?separatorP:"|"):"");
                strcat(tmpstr, tmpstr2);
                tmp = 1;
                operand &= ~SKP_MASK_SNI;
                indirect = 0;
            }
        }

        if( (operand & SKP_MASK_SMA) == SKP_MASK_SMA )
        {
            sprintf(tmpstr2,"%ssma%s", tmp?((asMacro)?separatorP:"|"):"", (indirect)?" i":"");
            strcat(tmpstr, tmpstr2);
            tmp = 1;
            operand &= ~SKP_MASK_SMA;
            indirect = 0;
        }

        if( (operand & SKP_MASK_SPA) == SKP_MASK_SPA )
        {
            sprintf(tmpstr2,"%sspa%s", tmp?((asMacro)?separatorP:"|"):"", (indirect)?" i":"");
            strcat(tmpstr, tmpstr2);
            tmp = 1;
            operand &= ~SKP_MASK_SPA;
            indirect = 0;
        }

        if( (operand & SKP_MASK_SPI) == SKP_MASK_SPI )
        {
            sprintf(tmpstr2,"%sspi%s", tmp?((asMacro)?separatorP:"|"):"", (indirect)?" i":"");
            strcat(tmpstr, tmpstr2);
            tmp = 1;
            operand &= ~SKP_MASK_SPI;
            indirect = 0;
        }

        if( (operand & SKP_MASK_SZA) == SKP_MASK_SZA )
        {
            sprintf(tmpstr2,"%ssza%s", tmp?((asMacro)?separatorP:"|"):"", (indirect)?" i":"");
            strcat(tmpstr, tmpstr2);
            tmp = 1;
            operand &= ~SKP_MASK_SZA;
            indirect = 0;
        }

        if( (operand & SKP_MASK_SZO) == SKP_MASK_SZO )
        {
            sprintf(tmpstr2,"%sszo%s", tmp?((asMacro)?separatorP:"|"):"", (indirect)?" i":"");
            strcat(tmpstr, tmpstr2);
            tmp = 1;
            indirect = 0;
            operand &= ~SKP_MASK_SZO;
        }

        if( operand & SKP_MASK_SZF )
        {
            sprintf(tmpstr2,"%sszf%s %0o", tmp?((asMacro)?separatorP:"|"):"", (indirect)?" i":"",
                (operand & SKP_MASK_SZF));
            strcat(tmpstr, tmpstr2);
            tmp = 1;
            operand &= ~SKP_MASK_SZF;
            indirect = 0;
        }

        if( operand & SKP_MASK_SZS )
        {
            sprintf(tmpstr2,"%sszs%s %o0", tmp?((asMacro)?separatorP:"|"):"", (indirect)?" i":"",
                (operand & SKP_MASK_SZS) >> 3);
            strcat(tmpstr, tmpstr2);
            tmp = 1;
            operand &= ~SKP_MASK_SZS;
            indirect = 0;
        }

        if( !tmpstr[0] || (operand != 0))    //  must be data or a bare skp
        {
            if( operand == 0 )
            {
                resultP += sprintf(resultP,"skp%s", (indirect)?" i":"");
            }
            else
            {
                resultP += sprintf(resultP,"%06o", word);
                flags = INSTR_NOTONE;
            }
        }
        else
        {
            resultP += sprintf(resultP,"%s", tmpstr);
        }
        break;

    case IS_OPR:
        // Like skips, multiple can be combined, so all special cases.
        // Macro1 allows an 'or' operation, 'a!b', which is used in macro mode.
        // However, the native assembler doesn't support this, so any combined operations
        // are emitted as the octal word with a comment.
        // If we see any extra bits not valid microinstuctions, then just the octal data is output.
        tmp = 0;                // set to 1 if we have printed one already
        tmpstr[0] = 0;          // be sure we start empty

        // A special case is nothing set, means nop
        // Check this first
        if( (operand & OPR_MASK_NOP) == 0 )
        {
            operand = 0;            // nothing left
            strcpy(tmpstr, "nop");
        }
        else
        {
            bits03 = operand & 07;              // needed for a few ops
            operand &= 017770;

            if( (operand & OPR_MASK_LAT) == OPR_MASK_LAT )    // MUST come befoe CLA! Only 2 bit directive.
            {
                operand &= ~OPR_MASK_LAT;
                sprintf(tmpstr2,"%slat", tmp?((asMacro)?separatorP:"|"):"");
                strcat(tmpstr, tmpstr2);
                tmp = 1;
            }
            if( operand & OPR_MASK_CLA )        // MUST come after LAT!
            {
                operand &= ~OPR_MASK_CLA;
                sprintf(tmpstr2,"%scla", tmp?((asMacro)?separatorP:"|"):"");
                strcat(tmpstr, tmpstr2);
                tmp = 1;
            }

            // 1D operations
            if( (tmp2 = operand & (OPR_MASK_LAI | OPR_MASK_LIA)) )
            {
                if( tmp2 == (OPR_MASK_LAI | OPR_MASK_LIA) )
                {
                    cP = (asMacro)?"760060":"lsw";
                }
                else if( tmp2 == OPR_MASK_LAI )
                {
                    cP = (asMacro)?"760040":"lai";
                }
                else
                {
                    cP = (asMacro)?"760020":"lia";
                }

                sprintf(tmpstr2,"%s%s", tmp?((asMacro)?separatorP:"|"):"", cP);
                strcat(tmpstr, tmpstr2);
                operand &= ~(OPR_MASK_LAI | OPR_MASK_LIA);
                tmp = 1;
            }

            if( operand & OPR_MASK_CLI )
            {
                operand &= ~OPR_MASK_CLI;
                sprintf(tmpstr2,"%scli", tmp?((asMacro)?separatorP:"|"):"");
                strcat(tmpstr, tmpstr2);
                tmp = 1;
            }
            if( operand & OPR_MASK_CMA )
            {
                operand &= ~OPR_MASK_CMA;
                sprintf(tmpstr2,"%scma", tmp?((asMacro)?separatorP:"|"):"");
                strcat(tmpstr, tmpstr2);
                tmp = 1;
            }
            if( operand & OPR_MASK_HLT )
            {
                operand &= ~OPR_MASK_HLT;
                sprintf(tmpstr2,"%shlt", tmp?((asMacro)?separatorP:"|"):"");
                strcat(tmpstr, tmpstr2);
                tmp = 1;
            }
            if( !(operand & OPR_MASK_STF) && bits03 )
            {
                // CLF is bits03, already cleared
                sprintf(tmpstr2,"%sclf %o", tmp?((asMacro)?separatorP:"|"):"", bits03);
                strcat(tmpstr, tmpstr2);
                bits03 = 0;                  // done with these
                tmp = 1;
            }
            if( (operand & OPR_MASK_STF) && bits03 )
            {
                operand &= ~OPR_MASK_STF;
                sprintf(tmpstr2,"%sstf %o", tmp?((asMacro)?separatorP:"|"):"", bits03);
                strcat(tmpstr, tmpstr2);
                bits03 = 0;                  // done with these
                tmp = 1;
            }
        }

        if( (operand | bits03) != 0 )                      // extra bits were left over, must be data
        {
            resultP += sprintf(resultP,"%06o", word);
            flags = INSTR_NOTONE;
        }
        else
        {
            resultP += sprintf(resultP,"%s", tmpstr);
        }
        break;

    case IS_OPR2:
        // Same ad-hoc needed as for OPR.
        // Neither macro or macro1 support any of these, so they are emitted as the octal word with a comment.
        // If we see any extra bits not valid microinstuctions, then just the octal data is output.
        tmp = 0;                // set to 1 if we have printed one already
        tmpstr[0] = 0;          // be sure we start empty

        if( asMacro )
        {
            // none are supported in macro or macro1
            resultP += sprintf(resultP,"%06o", word);
            break;
        }

        if( operand & OPR2_MASK_SCF )
        {
            operand &= ~OPR2_MASK_SCF;
            sprintf(tmpstr2,"%sscf", tmp?((asMacro)?separatorP:"|"):"");
            strcat(tmpstr, tmpstr2);
            tmp = 1;
        }
        if( operand & OPR2_MASK_SCI )
        {
            operand &= ~OPR2_MASK_SCI;
            sprintf(tmpstr2,"%ssci", tmp?((asMacro)?separatorP:"|"):"");
            strcat(tmpstr, tmpstr2);
            tmp = 1;
        }

        if( operand & OPR2_MASK_IFI )
        {
            operand &= ~OPR2_MASK_IFI;
            sprintf(tmpstr2,"%sifi", tmp?((asMacro)?separatorP:"|"):"");
            strcat(tmpstr, tmpstr2);
            tmp = 1;
        }

        if( operand & OPR2_MASK_IIF )
        {
            operand &= ~OPR2_MASK_IIF;
            sprintf(tmpstr2,"%siif", tmp?((asMacro)?separatorP:"|"):"");
            strcat(tmpstr, tmpstr2);
            tmp = 1;
        }

        if( operand & OPR2_MASK_IDA )
        {
            operand &= ~OPR2_MASK_IDA;
            sprintf(tmpstr2,"%sida", tmp?((asMacro)?separatorP:"|"):"");
            strcat(tmpstr, tmpstr2);
            tmp = 1;
        }

        if( operand != 0 )                      // extra bits were left over, must be data
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
        sP = findSpecial(iots, operand);
        extra = operand & ~sP->mask;             // extra bits not in the masked bits
        if( sP->notMacro && asMacro )
        {
            resultP += sprintf(resultP,"%s","iot");
        }
        else
        {
            resultP += sprintf(resultP,"%s", sP->name);
        }

        switch( (sP->notMacro && asMacro)?UNKNOWN:sP->modifiers )
        {
        case UNKNOWN:
            if( operand != 0 )                  // not an empty IOT
            {
                if( indirect )
                {
                    resultP += sprintf(resultP," i");
                    operand &= ~INDIRECT_BIT;
                }

                if( completion )
                {
                    resultP += sprintf(resultP," %s",(asMacro)?"4000":"C");
                    operand &= ~COMPLETION_BIT;
                }

                if( operand )
                {
                    resultP += sprintf(resultP,"%s%04o", (asMacro)?separatorP:" | ",  operand);
                    resultP += sprintf(resultP,"\t/%06o", word);
                }
            }
            else            // is ioh
            {
                if( indirect )
                {
                    resultP += sprintf(resultP," i");
                    extra &= ~INDIRECT_BIT;
                }

                if( completion )
                {
                    resultP += sprintf(resultP," %s",(asMacro)?"4000":"C");
                    extra &= ~COMPLETION_BIT;
                }
            }
            break;

        case CAN_WAIT:
            if( indirect )
            {
                resultP += sprintf(resultP," i");
                extra &= ~INDIRECT_BIT;
            }

            if( completion )
            {
                resultP += sprintf(resultP," %s",(asMacro)?"4000":"C");
                extra &= ~COMPLETION_BIT;
            }
            break;

        case INVERT_WAIT:       // because macro automatically sets, xx-i means not set
            if( !indirect )
            {
                resultP += sprintf(resultP,"-i");
            }

            extra &= ~INDIRECT_BIT;

            if( completion )
            {
                resultP += sprintf(resultP," %s",(asMacro)?"4000":"C");
                extra &= ~COMPLETION_BIT;
            }
            break;

        case IS_DPY:            // does INVERT_WAIT plus intensity
            if( !indirect )
            {
                resultP += sprintf(resultP,"-i");
            }

            extra &= ~INDIRECT_BIT;

            if( completion )
            {
                resultP += sprintf(resultP," %s",(asMacro)?"4000":"C");
                extra &= ~COMPLETION_BIT;
            }

            if( asMacro )
            {
                tmp = operand & 03700;  // macro doesn't print the intensity if it's zero 
            }
            else
            {
                tmp = operand & 03770;
            }

            if( tmp > 0 )
            {
                resultP += sprintf(resultP," %04o", tmp);
            }

            extra = 0;              // dpy uses some of the special bits
            break;

        case IS_IOH:            // special case for completion flag
            if( completion )
            {
                resultP += sprintf(resultP," %s",(asMacro)?"-i :4000":"C");
                extra &= ~COMPLETION_BIT;
            }
            break;

        case HAS_ID:            // some encode a subdevice, eg tape drive number
            resultP += sprintf(resultP," 0%0o", (operand >> 6) & 0177);
            extra = 0;
            break;
        }

        if( (sP->modifiers != UNKNOWN) && (extra != 0) )
        {
            resultP += sprintf(resultP,"%s0%o", (asMacro)?separatorP:" | ",  extra);   // add extra bits
        }
        break;
    }

    if( defP )
    {
        defP->flags |= flags;      // add any updates
    }

    return( resultP );
}

// This expects a full instruction word.
// True if the instruction can potentially do indirection.
bool
opCanIndirect(int word)
{
int opcode;
CodeDef *instructionP;

    opcode = word >> 13;
    if( opcode > 037 )
    {
        return(false);      // out of bounds
    }

    instructionP = &opcodes[opcode];
    return( (instructionP->modifiers == CAN_INDIRECT) );
}

// This expects a full instruction word.
// True if the instruction instance does an idirect operation.
bool
instructionIndirects(int word)
{
int opcode;
CodeDef *instructionP;

    opcode = word >> 13;
    if( opcode > 037 )
    {
        return(false);      // out of bounds
    }

    instructionP = &opcodes[opcode];
    return( (instructionP->modifiers == CAN_INDIRECT) && (word & INDIRECT_BIT) );
}


// This expects a full instruction word.
// True if the instruction reads or writes memory.
bool
opCanAccessMemory(int word)
{
int opcode;
CodeDef *instructionP;

    opcode = word >> 13;

    if( opcode > 077 )
    {
        return(false);      // out of bounds
    }

    instructionP = &opcodes[opcode];
    return( (instructionP->flags & (INSTR_READS | INSTR_WRITES))?true:false );
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
