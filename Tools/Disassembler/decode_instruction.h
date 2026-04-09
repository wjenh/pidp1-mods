#ifndef DECODE_INSTR_H
#define DECODE_INSTR_H
// This provides definitions for the CodeDef.flags field.
// A valid instruction that doesn't modify memory or change location such as law, cla, etc. just sets VALID.

#define INSTR_JUMPS      0x1   // is a jmp
#define INSTR_CALLS      0x2   // is a jsp or cal
#define INSTR_WRITES     0x4   // modifies memory
#define INSTR_READS      0x8   // reads memory, JUMPS and CALLS don't set this, but also check INDIR
#define INSTR_SKIPS      0x10  // any skip instruction
#define INSTR_XCT        0x20  // xct instruction, does random things
#define INSTR_NOTONE     0x40  // not a valid instruction, must be data
#define INSTR_VALID      0x80  // is a valid instruction
#define INSTR_INDIRECT   0x100 // set if the specific instruction instance has the indirect bit set
#define INSTR_JDA        0x200 // was a jda, not a jsp, jdr, or cal

// Gives instruction-specific details set in the CodeDef.modifiers field.
typedef enum {NONE, CAN_INDIRECT, IS_SKIP, IS_SHIFT, IS_OPR, IS_OPR2,
    IS_IOT, IS_IOH, IS_LAW, IS_CALJDA, IS_ILLEGAL} Modifiers;

// Defines one instruction.
// This is set per opcode in decode_instruction.c.
typedef struct
{
    char *name;             // printable instruction name
    Modifiers modifiers;    // see above
    int flags;              // see above
} CodeDef, *CodeDefP;

char *decodeInstr(int word, int addr, bool asMacro, char *separatorP, char *symbolP, char *resultP, CodeDefP defP);
bool opCanIndirect(int word);
bool instructionIndirects(int word);
bool opCanAccessMemory(int word);

#endif
