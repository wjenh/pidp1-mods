#ifndef DECODE_INSTR_H
#define DECODE_INSTR_H
// This provides definitions for the instruction type flags.
// A valid instruction that doesn't modify memory or change location such as law, cla, etc. just set VALID.

#define INSTR_JUMPS      0x1   // is a jmp
#define INSTR_CALLS      0x2   // is a jsp, jsr, or call
#define INSTR_WRITES     0x4   // modifies memory
#define INSTR_READS      0x8   // reads memory, JUMPS and CALLS don't set this, but also check INDIR
#define INSTR_SKIPS      0x10  // any skip instruction
#define INSTR_XCT        0x20  // xct instruction, does random things
#define INSTR_NOTONE     0x40  // not a valid instruction, must be data
#define INSTR_VALID      0x80  // is a valid instruction
#define INSTR_INDIRECT   0x100 // set if the specific instruction instance has the indirect bit set

#endif
