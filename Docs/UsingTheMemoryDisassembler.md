# disassemble_mem - a program to disassemble a memory image file produced by **pidp-1**

Updated 30-Mar-2026

## SYNOPSIS
```
disassemble_mem [-m|1] [-b nn] [-s nnnn] [-e nnnn] [-a nnnn] [-o filename] [memfile]
```
## DESCRIPTION
This is a two-pass disassembler for the memory image file produded by the **pidp-1** emulator,
found by default in the install directory and named *coremem*.

Three modes are supported.\
The default is to list the disassembled memory in an informative format, but this cannot be assembled.  

The second is to generate output that **macro_1** can assemble.\
However, if extended memory is used, the code will not work as expected, warnings will be included as
comments in the code.

The third, using the *-1* flag which is the numeral one, not a the letter l, will properly handle extended
memory and generate output that **am1** can assemble.

Any memory location that is an address targeted by an instruction is assigned
a label of the form 'Lnnn'.
The label will them be used in the instruction and also shown at the target location.

## OPTIONS
- '-1' - am1 output
- '-m' - macro output
- '-b nn' - specify the memory bank to disassemble from
- '-s nnnn' - specify the starting address in the bank to start disassembly from, the default is 0
- '-e nnnn' - specify the ending address in the bank, the default is 07750
- '-a nnnn' - specify an address to output as the assembler *start nnnn* address, the default is the *-s* address
- 'memfile' - specify the memory file to use, the default is */opt/pidp1-mods/coremem*

Numbers can be given as 0nnnn, octal, nnnn, decimal, 0xnnnn, hexadecimal, 0bnnnnn, binary.

## LIMITATIONS
This program must be used with some care. A memory image does not specify if a block of information is actually
PDP-1 instructions or if it's data. As an example, MACRO1 will produce a block for constants and andther
for variables. There is nothing to identify those blocks as such, that must be decided by the user.

## EMBEDDABLE DISASSEMBLER
The decondeInstruction.c code provides a standalone instruction disassembler that can be embedded in other code
that needs a readable version of a binary instruction word.

## BUILDING
Just do:
make

## ERRORS

The following errors are possible and will be reported on stderr after which an exit(1) is done.
- Incorrect command line arguments
- Can't open input file
- Can't open output file

## AUTHOR
Bill Ezell (wje)  
Send comments or changes to pidp1@quackers.net
## LICENSE
This software may be freely used for any purpose as long as the author credit is kept.
It is strongly asked that the revision history be updated and any changes sent back to pdp1@quackers.net so
the master source can be maintained.

A note on formatting:
This code uses the One Really True formatting style.
While it might appear verbose, as in braces around single if() bodies and braces on separate lines,
please follow it.
It is based upon some research into causes of errors in C done at Stanford many decades ago, refined
by use over 30+ years in a commercial environment both for C and Java.
And, yes, real programmers comment their code!
