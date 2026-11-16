## Using the Type 62 line printer

The Type 62 line printer printed 120 character wide lines from a 64 character set using FIODEC codes,
called flexo in macro, although was really Concise code, FIODEC without a parity bit.
It could print 600 lines per minute.

**NOTE** that the operating mode can be switched to Type 64 if desired.\
Use the lptType64 setting in the /opt/pidp1-mods/pidp1.config file.

Updated 16-Nov-2026
Clarifications added.

## The output

Line printer output is written to a file, */tmp/pdp1lpt.txt* by default, but see below.\
The file is opened in append mode if not already open and lines are written to it.\
The file is flushed when each line is printed, the file can be viewed or copied at will.

The file is closed if the file name is changed with *lpf* or by an *lpm* with its close bit set, see below.

If it can't be opened, every IOT call gives -1 in IO and what would be printed is discarded.
An IOT that fails with wait or completion completes at once.

In flexo mode, Concise characters are converted to ASCII with any character that has no equivalent printed as a space.
The conversion set is the same as used by **macro1**, **am1**, **DCS2**, and any other character conversions
provided in the pidp1-mods universe.

In ascii mode, the characters are used as-is.

## Timing

Timing is from the information in the PDP-1 Handbook.

Loading characters into the line printer buffer complete in one cycle, 5us.\
Printing a line takes 84 milliseconds.\
Spacing a line or lines takes 16 milliseconds per line, 1 line printed and spaced by 1 takes 100 milliseconds,
600 lines per minute.

There is some conflicting information, though.
The Type 62 manual for the PDP-4 states a spacing time of 7.5 msecs.
There are also varying descriptions of the Type 62 stating printing speeds of 600 lpm or 1000 lpm.
The above are used, since that is what is in the PDP-1 Handbook.

New characters could be transferred while line spacing was being done but not when the line was being printed.
However this implementation allows new characters to be sent as soon as the print line IOT, *prl*
is executed, no waiting is required.

But, see *Overstrike* below.

## Configuration

Configuration of several functions can be done in the pidp1.config file:

- lptType64=true/false
select Type 62 or Type 64 mode, default false, Type 62
- lptLines=nn
sets the mumber of lines per page, default 66
- lptLineSpacing=n0,n1,n2,n3,n4,n5,n6,n7, default 1,2,3,4,11,22,33,-1
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
This meant that the paper was not advanced when a *prl* IOT was issued, but the characters were printed.
Subsquent characters would be printed over existing characters on that line, overstruck.
The paper was only advanced when an *slp* was executed.

This can't be properly emulated because a text file is being created, and you can't overstrike in that.
For this implementation, *prl* resets the character position to the beginning of the line,
preserving the current contents of the print buffer.
New characters wil replace existing characters.

For example:
```
abcdef in buffer
prl
flb xyz
xyzdef now in buffer

slp
```
The output will be a line *xyzdef*.

## The IOTs

There are only three original plus two added, implemented as IOT 45.

- IOT 0045, prl, print the buffer, no line advance is done
- IOT 1045, flb, add the 3 characters in IO to the print buffer
- IOT 2x45, slp, space one or more lines
- IOT 3045, lpf, set printer output file
- IOT 3145, lpm, set ascii or flexo mode

These as well as values for spacing format control are in the <LPT/type62defs.ah> include file.

## The flb IOT

This adds the characters in IO,  2 in ascii mode or 3 in flexo mode, to the print buffer.
If the buffer overflows, extra characters are discarded.
The characters are packed as 3, 6 bit characters as produced by the assembler *flexo* directive
or packed as 2, 9 bit characters as produced by the **am1** *ascii* or quoted character directives.

```
721045

IO - contains 3 flexo characters, a<<12 | b<<6 | c in flexo mode, 2 ASCII characters a<<9 | b in ascii mode

On return, IO will be 0 for success, 0777776 (-1) if the output file can't be opened.
AC is unchanged.
```
Wait, i, and completion, C, are ignored.

In flexo mode, the word must have 3 characters, use a flexo space to pad it out.
This is done automatically when the assembler directives are used.

In ascii mode, either character can be a null, \\0, in which case it is ignored.

## The slp IOT

This command advances by the number of lines specifed in the IOT.
Until this command is issued, the same line can be overstruck.

In order to get the overstrike behavior, *prl*, below, doesn't actually output anything,
it just delays for 84 milliseconds.
Only when *slp* is executed is text written to the output file.
```
722x45

On return, IO will be 0 for success, 0777776 (-1) if the output file can't be opened.
AC is unchanged.
```
Wait, i, and completion, C, are supported and will take 18 * number-of-lines milliseconds to be issued.

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
of lines for each spacing.\
This is supported by the lptLineSpacing setting in the /opt/pidp1-mods/pidp1.config file.

Existing documentation has conflicting descriptions of the spacing tape function, some indicate
that all 8 formats can be defined, others that formats 0 and 7 are fixed to be overstrike and form-feed
respectively.
This implementation allows all to be changed.

## The prl IOT

In the original printer this printed the current buffer and cleared it but did not advance a line.
In order to duplicate the overstrike behavior, this IOT does nothing but
reset the buffer position to 0 and delay for 84 milliseconds.

The actual writing to the output file is done when *slp* is executed.

```
720045

On return, IO will be 0 for success, 0777776 (-1) if the output file can't be opened.
AC is unchanged.
```
Wait, i, and completion, C, are supported and will take 84 milliseconds to be issued.

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

It opens the file, creating it if necessary, before it returns.
If it fails, nothing is channged.
The file in use before is still in use.
A name can't be read if it has no null character within 128 words or is longer than 120 characters
after any ~ is expanded.

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

This is an added IOT for convenience.\
It changes the character mode between flexo and ascii.
The printer type is set only by the lptType64 setting.\
It can also close the print file.
```
723145

IO bit 17 - 0 for flexo mode, 1 for ascii mode
IO bit 16 - 0 for no operation, 1 to close the output file and reset the line number and flex shift state

On return, IO will be 0 for success, 0777776 (-1) if the output file can't be opened.
AC is unchanged.
```
Wait, i, and completion, C, are ignored.

If flexo mode is set, then the *flb* IOT will expect flexo/concise characters packed three per word.\
If ascii mode is set, then the *flb* IOT will expect ASCII characters packed two per word.

If the output file is closed, it will be reopened by the next printer IOT other than *lpf*,
which opens the file it is given instead.

The default is flexo mode.\
See *flb* for more details.
