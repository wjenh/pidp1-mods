## Using the Type 64 line printer

The Type 64 line printer printed 120 character wide lines from a 64 character set using fiodec codes,
called flexo in macro although was really concise code, which is fiodec without a parity bit.

Documentation has been scarce so some of the implementation is guesswork, but the implementation prints
using similar logic as the Type 62 printer used.

Updated 20-Mar-2026

## The output

Line printer output is written to a file, */tmp/pdp1lpt.txt* by default, but see below.\
The file is opened in append mode if not already open and lines are written to it.\
The file is flushed when each line is printed, the file can be moved or copied at will.

The file is closed when an *lpc* instruction is issued, see below.

In flexo mode, concise characters are converted to ascii with any character that has no equivalent printed as a space.
The conversion set is the same as used by **macro1**, **am1**, **DCS2**, and any other character conversions
provided in the pidp1-mods universe.

In ascii mode, the characters are used as-is.

## Timing

Timing is guesstimated from the information in the PDP-1 Handbook.

From that, the *lpc* instruction takes 5 milliseconds to complete, the *lpb* instruction takes one cycle,
5 microseconds, no delay, and the *pas* instruction takes 200 milliseconds (300 lines/min).

New characters can be transferred while the current line is being printed.

## Overstrike

The original printer supported overstriking.
This meant that the paper was not advanced when a *pas* instruction was issued, but the characters were printed.
Subsquent characters would be printed over existing characters on that line, overstruck.

This can't be properly emulated.
An overstruck line resets the character position to the beginning of the line, preserving the current contents
of the print buffer.
New characters wil replace existing characters.

For example:
```
abcdef in buffer
pas overstrike
xyz
xyzdef now in buffer
```

## The IOT instructions

There are only three original plus two added, implemented as IOT 45.

- IOT 2045, lpc, clear buffer, resets print buffer to empty
- IOT 0045, lpb, add the 3 characters in IO to the print buffer
- IOT 1x45, pas, print the buffer, x specifies the spacing
- IOT 3045, lpf, set printer output file
- IOT 3145, lpm, set ascii or flexo mode

## The lpc instruction

This resets the printer to an initial state.
Any characters in the buffer that have not been printed are discarded.
It also closes the printer output file.

The PDP-1 handbook calls this the *clr* instruction, but that seems too common a symbol name,
it is *lpc* in the **am1** *\<LPT\>/type64defs.ah* include file.
```
722045

IO and AC are unchanged.
```
Wait, i, and completion, C are supported and will take 5 milliseconds (1000 cycles) to be issued.

## The lpb instruction

This adds the 2 in ascii mode or 3 in flexo mode characters in the IO register to the print buffer.
If the buffer overflows, extra characters are discarded.
The characters are packed as 3, 6 bit characters as produced by the assembler *flexo* directive
or packed as 2, 9 bit characters as produced by the **am1** *ascii* or quoted character directives.

```
720045

IO - contains 3 flexo characters, a<<12 | b<<6 | c in flexo mode, 2 ascii characters a<<9 | b in ascii mode

On return, IO will be 0 for success, 0777776 (-1) if the buffer overflowed.
```
Wait, i, and completion, C are ignored.

In flexo mode, the word must have 3 characters, use a flexo space to pad it out.
This is done automatically when the assembler directives are used.

In ascii mode, either character can be a null, \\0, in which case it is ignored.

## The pas instruction

This command prints the current buffer and advances by the number of lines specifed in the instruction.
The buffer will then be empty.
```
721x45

IO and AC are unchanged.
```
Wait, i, and completion, C are supported and will take 200 milliseconds (40,000 cycles) to be issued.

The value of *x* selects one of 8 spacings:

- 0 no advance, the buffer point is reset to the beginning, all characters remain in it, *overstrike*
- 1 advance 1 line
- 2 advance 2 lines
- 3 advance 3 lines
- 4 advance 6 lines
- 5 advance 11 lines
- 6 advance 22 lines
- 7 insert a form-feed character after the current line

These do not match the Type 62 printer, it handles overstrike differently, but they are very similar.

## The lpf instruction

This is an added instruction because there is no actual line printer.
It allows setting of the output file to write to.
```
723045

IO - contains the address in memory of an ascii string giving the file name to use, or 0

On return, IO will be 0 for success, 0777776 (-1) if the file can't be opened.
AC is unchanged.
```
Wait, i, and completion, C are ignored.

If IO is 0, the file name is reset to the default */tmp/pdp1lpt.txt*.\
Otherwise the address is that of a string of ascii characters packed 2 per word as produced by the **am1**
*ascii* command.
The first character is in the high 9 bits of a word, the second in the low 9 bits.
For either, the high 9th bit is ignored.

Example in **am1** syntax:
```
    lio [fname
    lpf
    .
    .
    .

fname,
    ascii "/foo/bar/print.txt"
```
Note that the file name cannot contain wildcards or other *globbing* characters, those are expanded
by the shell.
However, relative paths can be used, *../b*.
In specific, the path must be one that linux *fopen()* accepts.

Also note that unless an absolute path is given, the file will be relative to the current working directory
of *the running pdp1 instance*, most likely */opt/pidp1-mods*, don't depend upon it.

## The lpm instruction

This is an added instruction for convenience.
It changes the character mode between flexo and ascii.
```
723145

IO - 0 for flexo mode, otherwise ascii mode

On return, the IO and AC are unchanged.
```
Wait, i, and completion, C are ignored.

If flexo mode is set, then the *lpb* instructionj will expect flexo/concise characters packed three per word.\
If ascii mode is set, then the *lpb* instruction will expect ascii characters packed two per word.

The default is flexo mode.\
See *lpb* for more details.
