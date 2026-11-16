## Using the Type 64 line printer

The Type 64 line printer printed 120 character wide lines from a 64 character set using fiodec codes,
called flexo in macro although was really Concise code, FIODEC without a parity bit.

Documentation is scarce so some of the implementation is guesswork, but the implementation prints
using similar logic as the Type 62 printer used.

**NOTE** that the operating mode must be switched to Type 64 from Type 62, Type 62 is the default.\
Use the lptType64 setting in the /opt/pidp1-mods/pidp1.config file.

Updated 16-Nov-2026
Clarifications added.

## The output

Line printer output is written to a file, */tmp/pdp1lpt.txt* by default, but see below.\
The file is opened in append mode if not already open and lines are written to it.\
The file is flushed when each line is printed, the file can be viewed or copied at will.

The file is closed when an *lpc* is issued, an *lpm* with its close bit set,
or the file name is changed with *lpf*, see below.

If it can't be opened, every IOT gives -1 in IO and what is printed is discarded.
An IOT that fails with wait or completion completes at once.

In flexo mode, Concise characters are converted to ASCII with any character that has no equivalent printed as a space.
The conversion set is the same as used by **macro1**, **am1**, **DCS2**, and any other character conversions
provided in the pidp1-mods universe.

In ascii mode, the characters are used as-is.

## Timing

Timing is guesstimated from the information in the PDP-1 Handbook.

From that, the *lpc* IOT takes 5 milliseconds to complete, the *lpb* IOT takes one cycle,
5 microseconds, no delay, and the *pas* IOT takes 200 milliseconds (300 lines/min).

The *pas* timing is not really correct, it should take a fixed amount of time to print the line, then a variable
amount of time for each line advance, but the timing information could not be found so this timing is the
time for printing one line and advance one line.

New characters can be transferred while the current line is being printed.

## Overstrike

The original printer supported overstriking.
This meant that the paper was not advanced when a *pas* IOT was issued, but the characters were printed.
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

## Configuration

Configuration of several functions can be done in the pidp1.config file:

- lptType64=true/false
select Type 62 or Type 64 mode, default false, Type 62 mode
- lptLines=nn
sets the mumber of lines per page, default 66
- lptLineSpacing=n0,n1,n2,n3,n4,n5,n6,n7, default 0,1,2,3,6,11,22,-1
sets the line spacing values used with *pas*
- lptNoFF=true/false
select between using repeated newlines or a form-feed character, default false

The number of lines determines the number of newlines used if the form-feed character is not being used.
It also determines the delay time when either form of form-feed is done.

The line spacing values are selected by the format number in the *pas* command.
A 0 means overstrike, a -1 means form-feed.

## The IOT IOTs

There are only three original plus two added, implemented as IOT 45.

- IOT 2045, lpc, clear buffer, resets print buffer to empty
- IOT 0045, lpb, add the 3 characters in IO to the print buffer
- IOT 1x45, pas, print the buffer, x specifies the spacing
- IOT 3045, lpf, set printer output file
- IOT 3145, lpm, set ascii or flexo mode

These as well as values for spacing format control are in the <LPT/type64defs.ah> include file.

## The lpc IOT

This resets the printer to an initial state.
Any characters in the buffer that have not been printed are discarded.
It also closes the printer output file.

The PDP-1 handbook calls this the *clr* IOT, but that seems too common a symbol name,
it is *lpc* in the **am1** *\<LPT\>/type64defs.ah* include file.
```
722045

On return, IO will be 0 for success, 0777776 (-1) if the output file can't be opened.
AC is unchanged.
```
Wait, i, and completion, C, are supported and will take 5 milliseconds (1000 cycles) to be issued.

## The lpb IOT

This adds thec haracters in the IO register, 2 for ascii mode or 3 for flexo mode, to the print buffer.
If the buffer overflows, extra characters are discarded.
The characters are packed as 3, 6 bit characters as produced by the assembler *flexo* directive
or packed as 2, 9 bit characters as produced by the **am1** *ascii* or quoted character directives.

```
720045

IO - contains 3 flexo characters, a<<12 | b<<6 | c in flexo mode, 2 ASCII characters a<<9 | b in ascii mode

On return, IO will be 0 for success, 0777776 (-1) if the output file can't be opened.
AC is unchanged.
```
Wait, i, and completion, C, are ignored.

In flexo mode, the word must have 3 characters.
Use a flexo space to pad it out.
This is done automatically when the assembler directives are used.

In ascii mode, either character can be a null, \\0, in which case it is ignored.

## The pas IOT

This command prints the current buffer and advances by the number of lines specifed in the IOT.
The buffer will then be empty.
```
721x45

On return, IO will be 0 for success, 0777776 (-1) if the output file can't be opened or written.
AC is unchanged.
```
Wait, i, and completion, C, are supported and will take 200 milliseconds (40,000 cycles) to be issued.

The value of *x* selects one of 8 spacings:

- 0 no advance, the buffer point is reset to the beginning, all characters remain in it, *overstrike*
- 1 advance 1 line
- 2 advance 2 lines
- 3 advance 3 lines
- 4 advance 6 lines
- 5 advance 11 lines
- 6 advance 22 lines
- 7 insert a form-feed character after the current line

The original printer had a changeable spacing tape so customers could configure the number
of lines for each advance.\
This is supported by the lptLineSpacing setting in the /opt/pidp1-mods/pidp1.config file.

## The lpf IOT

This is an added IOT because there is no actual line printer.
It allows setting of the output file to write to.
```
723045

IO - contains the address in memory of an ASCII string giving the file name to use, or 0

On return, IO will be 0 for success, 0777776 (-1) if the name can't be read or the file can't be opened.
AC is unchanged.
```
Wait, i, and completion, C, are ignored.

It opens the file named, creating it if necessary, before it returns.
If it fails, nothing is changed.
The file in use before is still in use.
A name can't be read if it has no zero character within 128 words, or is longer than 120 characters
once any ~ is expanded.

If IO is 0, the file name is reset to the default */tmp/pdp1lpt.txt*.\
Otherwise the address is that of a string of ASCII characters packed 2 per word as produced by the **am1**
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
However, ~/, ~username/, and relative paths can be used, *../b*.
In specific, the path must be one that linux *open()* accepts, with the addition of the ~ processing.
It must name a disk file, see *The output* above.

Also note that unless an absolute path is given, the file will be relative to the current working directory
of *the running pdp1 instance*, most likely */opt/pidp1-mods*, don't depend upon it.

## The lpm IOT

This is an added IOT for convenience.
It changes the character mode between flexo and ascii.
```
723145

IO bit 17 - 0 for flexo mode, 1 for ascii mode
IO bit 16 - 0 for no operation, 1 to close the output file

On return, IO will be 0 for success, 0777776 (-1) if the output file can't be opened.
AC is unchanged.
```
Wait, i, and completion, C, are ignored.

If flexo mode is set, then the *lpb* IOT will expect flexo/Concise characters packed three per word.\
If ascii mode is set, then the *lpb* IOT will expect ASCII characters packed two per word.

If the output file is closed, it will be reopened by the next printer IOT other than *lpf*,
which opens the file it names instead.

The default is flexo mode.\
See *lpb* for more details.
