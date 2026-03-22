## Using the Type 62 line printer

The Type 62 line printer printed 120 character wide lines from a 64 character set using fiodec codes,
called flexo in macro although was really concise code, which is fiodec without a parity bit.

It could print 600 lines per minute.

**NOTE** that the operating mode can be switched to Type 64 if desired.\
Use the lptType64 setting in the /opt/pidp1-mods/pidp1.config file.

Updated 22-Mar-2026

## The output

Line printer output is written to a file, */tmp/pdp1lpt.txt* by default, but see below.\
The file is opened in append mode if not already open and lines are written to it.\
The file is flushed when each line is printed, the file can be viewed or copied at will.

The file is closed if the file name is changed, the mode is changed, or a file close command is used, see below.

In flexo mode, concise characters are converted to ascii with any character that has no equivalent printed as a space.
The conversion set is the same as used by **macro1**, **am1**, **DCS2**, and any other character conversions
provided in the pidp1-mods universe.

In ascii mode, the characters are used as-is.

## Timing

Timing is from the information in the PDP-1 Handbook.

Loading characters into the line printer buffer complete in one cycle, 5us.\
Printing a line takes 84 milliseconds.\
Spacint a line or lines takes 16 milliseconds per line, 1 line printed and spaced by 1 takes 100 milliseconds,
600 lines per minute.

New characters could be transferred while line spacing is being done, but this implementation allows that as
soon as the print line instruction is executed, no waiting is required.

## Configuration

Configuration of several functions can be done in the pidp1.config file:

- lptType64=true/false
select Type 62 or Type 64 mode, default false
- lptLines=nn
sets the mumber of lines per page, default 66
- lptLineSpacing=n0,n1,n2,n3,n4,n5,n6,n7, default 1,2,3,4,11,22,-1
sets the line spacing values used with *slp*
- lptNoFF=true/false
select between using repeated newlines or a form-feed character, default false

The number of lines determines the number of newlines used if the form-feed character is not being used.
It also determines the delay time when either form of form-feed is done.

The line spacing values are selected by the format number in the *slp* command.
A 0 means overstrike, but this is not needed with the Type 62 printer since that function
is performed by the *prl* command, see below.

A -1 means form-feed.

## Overstrike

The original printer supported overstriking.
This meant that the paper was not advanced when a *prl* instruction was issued, but the characters were printed.
Subsquent characters would be printed over existing characters on that line, overstruck.

This can't be properly emulated.
An overstruck line resets the character position to the beginning of the line, preserving the current contents
of the print buffer.
New characters wil replace existing characters.

For example:
```
abcdef in buffer
prl overstrike
xyz
xyzdef now in buffer
```

## The IOT instructions

There are only three original plus two added, implemented as IOT 45.

- IOT 0045, prl, print the buffer, no line advance is done
- IOT 1045, flb, add the 3 characters in IO to the print buffer
- IOT 2x45, slp, space one or more lines
- IOT 3045, lpf, set printer output file
- IOT 3145, lpm, set ascii or flexo mode

These as well as values for spacing format control are in the <LPT/type62defs.ah> include file.

## The flb instruction

This adds the 2 in ascii mode or 3 in flexo mode characters in the IO register to the print buffer.
If the buffer overflows, extra characters are discarded.
The characters are packed as 3, 6 bit characters as produced by the assembler *flexo* directive
or packed as 2, 9 bit characters as produced by the **am1** *ascii* or quoted character directives.

```
721045

IO - contains 3 flexo characters, a<<12 | b<<6 | c in flexo mode, 2 ascii characters a<<9 | b in ascii mode

On return, IO will be 0 for success, 0777776 (-1) if the buffer overflowed.
```
Wait, i, and completion, C are ignored.

In flexo mode, the word must have 3 characters, use a flexo space to pad it out.
This is done automatically when the assembler directives are used.

In ascii mode, either character can be a null, \\0, in which case it is ignored.

## The slp instruction

This command advances by the number of lines specifed in the instruction.
Until this command is issued, the same line can be overstruck.

In order to get the overstrike behavior, *prl*, below, doesn't actually output anything,
it just delays for 84 milliseconds.
Only when *slp* is executed is text written to the output file.
```
722x45

IO and AC are unchanged.
```
Wait, i, and completion, C are supported and will take 18 * number-of-lines milliseconds to be issued.

The value of *x* selects one of 8 spacings, the default values are:

- 0 advance 1 line
- 1 advance 2 lines
- 2 advance 3 lines
- 3 advance 4 lines
- 4 advance 11 lines
- 5 advance 22 lines
- 6 advance 33 lines
- 7 insert a form-feed character after the current line

The original printer had a changeable spacing tape so customers could configure the number
of lines for each advance.\
This is supported by the lptLineSpacing setting in the /opt/pidp1-mods/pidp1.config file.

## The prl instruction

In the original printer, this printed the current buffer and cleared it, but did not advance a line.
In order to duplicate the overstrike behavior, this instructon does nothing but
reset the buffer position to 0 and delay for 84 milliseconds.

The actual writing to the output file is done when *slp* is executed.

```
720045

IO and AC are unchanged.
```
Wait, i, and completion, C are supported and will take (18 * number-of-lines) milliseconds to be issued.

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

This is an added instruction for convenience.\
It changes the character mode between flexo and ascii and the printer mode between Type 62 and Type 64.\
It can also close the print file.
```
723145

IO bit 17 - 0 for flexo mode, 1 for ascii mode
IO bit 16 - 0 for no operation, 1 to close the output file and reset the line number and flex shift state

On return, the IO and AC are unchanged.
```
Wait, i, and completion, C are ignored.

If flexo mode is set, then the *flb* instruction will expect flexo/concise characters packed three per word.\
If ascii mode is set, then the *flb* instruction will expect ascii characters packed two per word.

If the output file is closed, it will be reopened when the next *slp* is executed.

The default is flexo mode.\
See *flb* for more details.
