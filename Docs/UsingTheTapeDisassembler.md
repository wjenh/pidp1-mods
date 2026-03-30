# disassemble_tape - a program to disassemble a PDP-1 binary prgram tape

Updated 30-Mar-2026

## SYNOPSIS
```
disassemble_tape  [-a] [-m] [-k] [-l] [-r] [-d] tapeimagefile
```
## DESCRIPTION
This is a two-pass disassembler for PDP-1 tape image files that contain RIM and BIN format data
as would be produced by the PDP-1 macro assembler.

It can also read tapes with the **am1** assembler loader.

All non-error output goes to stdout.  

Three modes are supported.\
The default is to list the disassembled tape in an informative format, but this cannot be assembled.  
Macro mode emits source that can be assembled by the MACRO1 and derivative assemblers.  

The second is to generate output that **macro_1** or **am1** can assemble.

The third just outputs the binary words as octal, one word per line.

For default and macro modes, any memory location that is an address targeted by an instruction is assigned
a label of the form 'Lnnn'.
The label will them be used in the instruction and also shown at the target location.

## OPTIONS
- '-a' - am1 extended loader compatibility, use for any am1 binaries
- '-m' - macro mode, output is compatible with the MACRO1 assembler
- '-c' - native macro mode, output is compatible with the native PDP-1 macro assembler
- '-k' - if in macro mode an initial RIM code block will not be output because MACRO usuaally does it; this keeps it
- '-l' - if in verbose mode, don't print the leader
- '-r' - Raw mode, just dump the tape as instructions or binary words, no RIM or BIN searching (not for macro)
- '-d' - debug mode, not useful except for debugging disassemble_tape
- Options can be combined, e.g. -mi.

## LIMITATIONS
This program must be used with some care. A binary tape does not specify if a block of information is actually
PDP-1 instructions or if it's data. As an example, MACRO1 will produce a block for constants and andther
for variables. There is nothing to identify those blocks as such, that must be decided by the user.
It expects compliance with the standard macro assembler output, a RIM block followed by one or more BIN blocks
terminated by a JMP, wich was produced by the macro 'start' directive.  

WHen the program starts, it searches for the pattern that identifies the RIM block.
Normally, any tape character that does not have bit 0200 set is ignored,
all binary words are 18 bits and consist of 3 characters with bit 0200 set.
Any non-binary character is ignored, even if in a word sequence.

However, any non-binary characters that are seen up to the first binary character on a tape are considered to be
part of a label for the tape and will be output in the form they would appear if you were looking at a physical tape.

If in either macro mode, if a JMP is seen following a BIN block, that indicates the end of data as far as the
macro assembler is concerned, no further data will be emitted.  

In default mode, if there are binary characters outside of a RIM or BIN block
they are assumed to comprise 18 bit words and are also disassembled.

## EMBEDDABLE DISASSEMBLER
The decondeInstruction.c code provides a standalone instruction disassembler that can be embedded in other code
that needs a readable version of a binary instruction word.

## ERRORS

The following errors are possible and will be reported on stderr after which an exit(1) is done.
- Incorrect command line arguments
- Can't open input file
- Nonstandard tape
- Unexpected data seen outside a RIM or BIN block (macro modes only)
- Premature EOF

## BUILDING
Just do:
make

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
