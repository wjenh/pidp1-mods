# Using the **ad1** symbolic debugger for the PDP-1

This document describes the **ad1** symbolic debugger and how to use it.

This is version 1.10 and covers up through ad1 version 1.14; it will be updated as needed.\
Edit date 5-Apr-2026

## What is **ad1**?

(Yet) Another Debugger for the PDP-1.

Why do we need another one?

While **DDT** was revolutionary for its time, its time has long past.

It was memory resident and consumed at least 25% of bank 0 memory.\
It required tedious procedures to set up and use.\
It was limited in the number of breakpoints it could set, one.\
Its command structure was, well, a bit obscure.

To be fair, many of these issues were a result of the very limited memory available.

## The features

One major difference from **DDT**, aside from the obvious added features,
is that **ad1** is not memory resident in the pidp-1's memory.
It is a standalone C program that interacts with the pidp-1 via a shared memory segment and hence
does not need to be loaded nor does it consume any pidp-1 memory at all.

Features
- Uses symbol tables from **am1** to provide symbolic names for program locations and variables
- Uses listing files from **am1** to provide viewing of the source by address, symbol, or line number
- Multiple files can be open to allow debugging of programs with separately assembled components
- Can set up to 8 breakpoints, configurable to any number at build time
- Individual breakpoints can be enabled, disabled, or deleted
- Breakpoints can have a hit count and won't be raised until the count is reached
- Can set up to 8 watches, configurable to any number at build time
- Individual watches can be enabled, disabled, or deleted
- Watches can have a value to match on write, or match on any write
- Memory locations can be interrogated and set
- Has full read access to the PC, PF, SS, TW, TA, MA, MB, IO, and AC registers
- Can set AC, IO, PC, and PF
- Can read the test switches and sense switches
- Fully supports multi-bank operations
- Numeric values can be in base 2, 8, 10, and 16
- A numeric value can override the base setting by specifying its base as in 0177, 0d23, 0xFF, 0b11001
- Data can be viewed as numbers including in 1's complement, symbols, instructions, ascii, or flex/concise
- Can directly load bin and am1 binary tapes, no mount or read-in needed
- Has built-in help for commands

## What goes on inside?

It is implemented in **C** using **Flex** and **Bison** for command parsing.

Central to its operation is the use of a shared memory segment that contains the full operating state of
the pidp-1.
This becomes available when the pidp-1 is started in *shared* mode via its configuration file.
This does impose the restriction that **ad1** must be running on the same machine as the pidp-1 instance
being debugged.

This is not only used for interrogating and setting the above registers and memory, it also allows control over
starting, stopping, single-stepping and processing of breakpoints and watches.

Commands are handled in the pidp-1 by *switch spoofing*.
Just after the state of the panel switches is checked, they are overriden by any **ad1** commands that
control the emulator state.
In this way, the behavior is exactly as if the panel switches had been used.

**Ad1** operates totally asynchnronously, all operations can be done while the pidp-1 is running.

Breakpoints are implemented by a breakpoint table in shared memory.
When breakpoints are active, an assignemnt to the PC register is checked to see if it is a breakpoint address
and if so and its hit count is reached, a flag is set to indicate that to **ad1** and the pidp-1 halted.

Watchpoints are implemented similarly with a watch table in shared memory.
When watches are active, the addresses have their contents checked to see if they have changed and
optionally match a given value.
If so a flag is set to indicate that to **ad1** and the pidp-1 halted.

Both breakpoints and watches are checked in the pidp-1 at the end of every machine cycle.

**Ad1** commands can then be given and when ready the program resumed.

No modification of program memory is done or needed, unlike the traditional **DDT** method of modifying instructions.
This also means a breakpoint *can* be set on an instruction that is modified by the program,
the breakpoint only depends upon the PC address.

Deleting a breakpoint just clears the associated entry in the breakpoint table.

However, as is the same for **DDT**, there is a restriction and for the same reason.\
A breakpoint cannot be set on data, since that will not be addressed by the PC register.\
Instead, a watch can be used since it only cares about the memory contents.

If a symbol table is given, it is parsed to build a map of symbol names to addresses and vice-versa.
If an address has an associated symbol, the symbol will be displayed. If a symbol is given, it is used
as an address.

Similarly, if a listing file is given, a map of addresses to line numbers is built.
Lines from the file can be searched by address, symbol name, or line number and displayed.

If only a source file is available or the program was not assembled by **am1**,
lines can be displayed by line number.

## Usage

ad1 [-v] [-y] [-x] [-T] [filename ...]

The **-v** option prints the version number and exits.

The **-y** option enables yacc/Bison debugging output, not generally useful.

The **-x** option enables lex/Flex debugging output, not generally useful.

The **-T** option starts in test mode. No connection to a running pidp-1 is made, an in-memory empty
state container is used instead.
An attempt is made to load the test memory with the last saved actual pidp-1 coremem save file from
the */opt/pidp1-mods* directory.

If no filename is given, then no symbols or source will be available until a file is specified by the *file* command.

Otherwise the filename can be one of:
- filename
- filename.am1
- filename.lst
- filename.sym

The three **am1** filenames, *.am1*, *.lst*, and *.sym* will be derived from whichever of the above is given.

If a filename.sym file is found, then the symbols will be available for use.\
If a filename.lst file is found, then the source list operations will be available for use.\
If there is no .lst file but there is a .am1 file, then source can be listed by line number only.

Multiple filenames can be given.
If so, the same information will be available for them.

For full functionality, programs should be assembled using the **am1** *-d* flag.

Note that the *shared* option must be on in the pidp-1 configuration file in order to be useful.

## Building it

As usual, just type 'make'. This assumes you have installed the pidp1-mods release and let it
install Flex and Bison.

## Numeric values

How numbers are entered and their base depends upon context.

For expressions, see below, and addresses a number can be in base 2, 8, 10, or 16.
The default is set by the *base* command, initially base 8.

A number can also have a base specifier to explicitly give the base regardless of the base setting.
These prefix a number and have the form:
- 0b Binary, the digits 0-1 are allowed
- 0o Octal, the digits 0-7 are allowed
- 0d Decimal, the digits 0-9 are allowed
- 0x Hexadecimal, the digits 0-9A-Fa-f are allowed

For example, *break 0x4F*.

For some parameters to commands, the number is *always* decimal, any base setting is ignored, a base specifier
is not allowed.

For example, a breakpoint number is always decimal, but a breakpoint address can be in any of the allowed bases.

## Line numbers, addresses, and symbols

What is available depends upoon which of the filenames mentioned above are present and how many files
have been opened.

If there is a listing file, then a mapping between line numbers *in that listing file* and memory locations
shown in the file is created.

For example, this line from a listing file:
```
11: 000106 600104 jmp loop
```
says that at line 11 in the *original* source file the instruction is at memory location 000106.
But, when a listing file is available, lines from it are what will be displayed, and the line number
does not necessarily match line 11, especially if there are include files.

When a listing file is used, the line number shown is ignored and the actual line number in the listing
file is used for creating the map and, if a symbol file is also being used, to set the lines for the symbols.

Sometimes multiple lines have the same address, such as when multiple instructions are given on the same line.
In this case, listing that address lists all lines for that address.

If only a source file is being used, then the line numbers will be from that file and if a symbol file
is also being used, the line nubmers from the symbol file will be used for the symbol locations.

If multiple files have been loaded, they should not have overlapping line numbers or duplicated symbols
*unless* they are in different banks.

If not, use of the *file* command or some *list* commands can be used to switch between the overlapping files.

At any time, there is a current file, initially the first file.
Several commands change the current file either explicitly, such as the *file* command.
Some other commands that reference a symbol, prints the value at an address, or lists at an address can
change it also, as can breakpoints and watches.

See the section on **File switching**.

Line numbers are always decimal.

## Ones complement, twos complement, oh my

The PDP-1 used 1's complement math, which no modern computers use.
But, **ad1** runs on a modern computer and uses 2's complement math.

However, in order for the math and bitwise operations to give the same result as they would have then,
**ad1** automatically converts the operands(s) of the math operations *+, -, \*, /, %%* and unary minus to
the 2's complement representation, performs the operation, and converts back to the 1's complement
representation.
The values that are stored by the *set* command will be the correct 1's complement values.

Remember that one annoying thing about 1's complement is that there are two values for zero, 0 and -0.
0 is all bits off, -0 is all bits on.
The PDP-1 automatically converted -0 to 0 for the *add*, *sub*, *mul*, *div*, *idx*, and *isp* instructions.
This is also done when values are computed in **ad1** as noted above.

Logical operations, *&*, *|*, *^*, *~*, are not math operations and do not do zero or 2's complement computations.

When values are displayed, they are shown as unsigned 18 bit values, just as you would see in a program listing.
The PDP-1 front panel didn't show signed digits, it just showed binary light patterns.

The one exception is the special *c* formatting directive for the *show* command, see below.

## Addresses and extended memory

Because **ad1** can debug code in any memory bank, addresses are always full 16-bit values.
This can be confusing because of the way the PDP-1 treated addressing within an extended memory bank.
Regardless of the bank being executed in, addresses other than indirect addresses were treated as 12-bit addresses
in the current bank.

For example, a *jmp 123* in bank 1 is exactly the same code as a *jmp 123* in bank 0.
But, the actual address in bank 1 is *010123*.
This normally won't cause a problem, but there is one case where addressing is important.

Symbols with the same name can exist in different banks with different values.
**ad1** keeps symbols with their full address, provided in the symbol table produced by **am1**.

An example, assume a location in bank 1 named *loc*, and a location in bank 0 also named *loc*.
Which one is the one to use?

This is handled in two ways.

First, a default current bank can be set with the *bank* command. All unqualified symbol lookups will
then only match symbols in that bank.

Second, a *bank qualifier* can be added to a symbol name, e.g., *loc,1* to explicitly identify the bank.\
Note that if multiple files are open, additional behavior applies.

All other uses of an address automatically add the current bank if the address appears to be in bank 0.
This is consistent with the way the PDP-1 (and pidp-1) deal with addresses in extended memory.
If you want to address the actual bank 0, you can change banks to bank 0 or use a bank referemce,
e.g. *break 100,0*.

A bank reference can be applied to any expression used as an address and overrides any other bank setting.

The action is that the expression is changed thus:
```
expression = (bank-ref << 12) | (expression & 07777)
```

## Expressions

In general, anywhere a numeric value or address is needed, an expression can be used, e.g:

```
start 100
start foo
start foo+1
show .+5
```

An expression is composed of numbers, symbol names, special symbols and math operators.
A regular symbol is the same as used in the assembler, a *location assignment*.
The value of a symbol is the memory address where storage for it is allocated.

The special symbol . (period, dot), means *the last address used*, similar to its use in an assembler.

## Operators

The following operators are defined and listed in order of increasing priority.\
The priority is the same as that for **C** and the operations are the same.\
Note that unlike **am1**, the mod operation is a single percent sign.

| Operator | Meaning                |
|----------|------------------------|
| \|       | bitwise or             |
| ^        | bitwise xor            |
| &        | bitwise and            |
| << >>    | left-shift right-shift |
| \+ \-    | addition subtraction   |
| \* / %   | multiply divide mod    |
| ~        | bitwise complement     |
| -        | unary minus, -n        |
| ( )      | expression nesting     |
| ,number  | bank reference         |

## Registers

Several commands can reference registers in the pidp-1.
The register names are:

- ac the AC register
- io the IO register
- pc the PC register
- ma the memory address register
- mb the memory buffer register
- as the address switches including the extended address
- tw the test word switches
- pf, pf1-6 the program flags
- ss the sense switches

Of these, the ac, io, pc, and pf can be modified via the *set* command.

Using pf refers to all 6 program flags, pf1-6 to each individual flag.

The three pseudo-registers *sy*, *break*, and *watch* are used by the *show* command, which see.

## The commands

The general form of a command is:
```
command
command [optional arguments]
command arguments [optional arguments]
```

Commands can generally be shortened, the number of characters required depends upon the command.
There is built-in help for the commands, and it will show the minimum length required.

For the commands below, all are lower-case. The upper case letters indicate the minimum length
for the command.

Arguments are in several forms. The names used in the descriptions means:

- decimal, a decimal-only integer number
- integer, an integer number in any of the allowed bases
- value, an integer , or,
- value, a symbol imported from an **am1** symbol file, with an optional *bank qualifier*, *symbol,banknum*
- expression, as noted aboe
- special symbol, as noted in the command description
- address, a positive value or expression within the memory bounds of the pidp-1, 16 banks of 4096 words.

Again, **ad1** operates by switch spoofing.

If the actual single-step or single-cycle switch is on, it takes priority and cannot be overridden.\
However, if the single-step switch is off and **ad1** is in step mode, setting it on and back off again
will effectively turn off step mode.

The remaining switches will operate as expected, they are momentary-action switches and are also handled
as such by **ad1**..

## Help [command-name or topic]

With no argument, a list of all commands and topics is shown with the minimum length needed indicated.\
If a command name or topic is given, the specific help for that is shown.

## BASe decimal

Sets the default base that numbers will use when typed in.

The valid choices are 2, 8, 10, and 16.\
This is overriden by an explicit base in the number.\
The initial base is 8.

This command always interprets the number in *base 10*.

It also changes the current output format to the new base.

## BAnk integer

Sets the default memory bank in use, 0-15 in decimal or the equvalent in other bases.

When set to any value other than zero, all addresses that are < 4096 in decimal are assumed to be in this bank.\
A base qualifier, *val,basenum*, overrides this setting.

## Break address [count]

Sets a breakpoint at the given address with an optional count of how many times it must be hit before it is reported.
The breakpoint is automatically enabled.

This command always interprets the count in *base 10*.

When reported, the pidp-1 is halted.\

Breakpoints are based on the address in the PC, so breakpoints placed in memory not referenced by the PC will never
be hit, such as data or variables. Use a watch instead.

## Continue

This is equivalent to using the continue switch on the front panel.
If the pidp-1 is halted, execution is resumed.

## DElete [Watch] [decimal]

If the decimal is the number of a set breakpoint, delete it.\
If no decimal is given, a prompt is given and if confirmed deletes all breakpoints.

This command always interprets the number in *base 10*.

If watch is specified, then this applies to watches instead of breakpoints.
It can be shortened the same as when used as a command.

## DIsable [Watch] [decimal]

If the decimal is the number of a set breakpoint, disable but don't delete it.\
It will not be procoessed if encountered until it is enabled again.

This command always interprets the number in *base 10*.

If watch is specified, then this applies to watches instead of breakpoints.
It can be shortened the same as when used as a command.

## Enable [Watch] [decimal]

If the decimal is the number of a set breakpoint, enable it if it is enabled.\
It will again be procoessed if encountered.

This command always interprets the number in *base 10*.

If watch is specified, then this applies to watches instead of breakpoints.
It can be shortened the same as when used as a command.

## FIle [n] | [[+]name]

If no argument is given, the current file number is listed along with all the open files with their file number.

If a number is given and it is that of a valid open file, then the current file is switched to that file.

Otherwise it opens a new file for use with **ad1*.

The name can be the basename of a program file, or have an *.am1*, *.rim*, *.sym*, or *.lst* extension.
The actual names needed will be derived, name.lst or name.am1, and name.sym.

If the name is prefixed by a plus sign, *+*, then the file is opened and added to the list of files.
If only a name is given, the current files, if any, are first closed. Any symbols and line maps are cleared.

In both cases, symbols will be loaded if a .sym file is available,
and line-address mapping provided if a .lst file is available.

The source will be shown from the .lst file, or if not found, the .am1 file.
If neither was found, no source will be available.

Symbols are always identified by the combination of bank number and file number.

If a file has the same line number(s) in the same bank as another opened file,
a line lookup by address will first look in the current file and if not found there, will change
the current file to the first file containing that address, if the address was found.\
If a symbol of the name name in the same bank as as another symbol is found, the same us done.

The addresses and files of all symbols can be seen using the *list sy* command.

The name is entered without any quotes. \
The name can contain a file path.\
If the file can't be found, an error will be reported.

This is the same as if the name was given on the command line when **ad1** was started.

## FOrmat [format]

If no argument is given, the current format is shown.\
Otherwise, the current format is set to the one given.

The format is the same as for the *SHow* command below.

## List [decimal | expression | @expression[,bref][:fileno] | symbol[,bref][:fileno] | .[+-decimal]]

If a source or listing file is open and is the current file, list lines of text from it.\
If no argument is given, list from the next line after the last one listed, or if none, the first line.\
If the argument is a decimal, it is the line number to list.\
If the argument is an expression, it is the line number to list, but see below.

If it is a symbol, it is the line number of the line the symbol was assigned a location.
The symbol might not have been in the current file, in which case the current file is made the file
the symbol is defined in.

If an expression is preceeded by @, then it is the line that corresponds to that address in the listing file,
*line at address*.\
Again, if the address corresponds to that in another file, the current file will be made the file the address
is defined in.

if a . (period, dot) is given, it means *list from the line corresponding to the last address used*.\
It can have an optional + or - decimal, which means the last address plus or minus that value,
e.g. *li .+4*.\
This also can change the current file.

If an empty line is entered, it is equivalent to entering this command with no arguments.

If the file was just a source file, then lines can only be viewed by line number.\
Otherwise, if the **ad1** .lst file is found, full functionality is available.

When the file is a listing file, the line numbers in it correspond to the original input source and
aren't usually the same as the line in the listing file, they refer to the line in whichever file is being
listed, so includes and macros can skew it.

The list command displays the line number in the listing file itself, the file you're viewing, and the same
for just a source file.

If the address being shown has been modified by the program, the modified value will not be seen since
the line is coming from the listing file.
To see the actual value, use the *show .* command.

For example:
```
23   42: jmp foo
```
The first number is the line in the file being viewed and is the line a *list at nn* command will show.

## LOad name

Attept to load the named file as a binary or am1 rim tape.

If successful, the current start address is set to the starting address unless the tape was an am1
tape with a *stop* directive, in which case no starting address is set.

## Next

Repeat the last *show* command at the next address location.
This does not change the pidp-1's PC, it is **ad1**'s local address location.

## Quit and Exit

These both terminate **ad1**, but with an important difference.

Quit deletes any breakpoints and watches and **ad1** exits.

Exit **preserves* any breakpoints and watches bud disables them and **ad1** exits.\
However, they are preserved only until the pidp-1 is restarted.

Any normal termination other than via exit is equivalent to quit, all watches and breakpoints are deleted.\
A kill via a signal will leave all breakpoints and watches in whatever state they are in.

The pidp-1 is left in whatever state it is in.

## SEt address|register value

The location in memory or the given register is set to the value.\
If the value is a negative number, it is adjusted to the 1's complement equivalent and masked to 18 bits.

If a register is given, it is similarly set, but if the register is a program flag, it can only be set to 1 or 0.\
See the section on *registers* for more details.

This is roughly equivalent to using the deposit switch on the front panel.

## SHow address|register [format]

Shows the contents of a memory address or register with an optional format modifier.\
For an address, this is roughly equivalent to using the examine switch on the pidp-1 front panel.
IF symbol is prefixed with *#*, then its address, not its contents, will be shown.

See the section on *registers* for more details on registers.

An optional format can be given, and is one of:

- b show the value in binary
- o show the value in octal
- d show the value in decimal
- x show the value in hexadecimal
- c treat the value as a 1's complement 18 bit number, print the equivalent negative number if the value is negative
- s show the value as a symbol name if a symbol address has that value, else as an octal number
- i show the value as an instruction
- a show the value as 2 ascii characters, the first in the high 9 bits, the second in the low 9 bits
- f show the value as 3 flex characters, packed as 6 bits each

There is a pseudo-register *sy* that will list all the loaded symbols and their addresses.

There are two other pseudo-registers, *break* and *watch*,
which can be shortened in the same way as the command name.
These will list all set breakpoints or watches and their status.

Displaying the break, watch, pf, or ss registers ignores any optional format.
The pf and the the ss registers are always displayed in binary.

The *a* and *f* formats use the same value formats as the **ad1** 'c', *ascii*, *flex*, and *text* instructions
produce.

This is (very) roughly equivalent to using the examine switch on the front panel.

## STArt [address]

This is equivalent to entering a start address on the pidp-1 front panel and using the start switch.
The address is a full 16-bit address, so execution can start in any bank.

If no address is given, the last start address set by this command or by *load* is used.
If there has not been an address set, an error will be reported.

## Step [count]

This is equivalent to using the continue switch on the front panel while the single-instruction switch is on.\
If *count* is given, that many number of instruction cycles is executed.\
If *count* is not given, it defaults to 1.

If *count* is greater than 1 and a breakpoint or watchpoint is hit, stepping is immediately stopped
and normal breakpoint or watchpoint handling is done.

If there is a line in the listing file, if present, that matches the pc address after stepping is done,
it will be displayed.

Note that the shift and rotate instructions can extend into the next cycle, so the value you see
might not be what you expect.
This is normal and you would see the same by single-instruction stepping using the actual panel.

Specifically, if a shift or rotate of more than 6 bits is done, the remaining bits are shifted/rotated
at the beginning of the next cycle.

## STOp

This is equivalent to using the stop switch on the front panel, the pidp-1 is halted.

## Trace

If the instruction at the current address reads, writes, or transfers to its address part, show
the target address.

If the instruction is an indirect operation, automatically indirect to the target location.
If that location is marked as indirect, ask whether or not to follow it.
Repeat until a non-indirect target is found.

If extended memory mode is enabled, then the indirection following past the first location the instruction
indirects to is not done, because indirection is only one level in this mode.

For each address, if there is an associated symbol, it is shown.
Otherwise, the address is shown in the current numeric format.

Finally, ask whether or not to make the final address the current address.

## Watch address [value]

Sets a watch at the given address with an optional value to watch for.

If a value is given, a change of the value in memory to this value signals the watch.\
If no value is given, then any change of the value in memory signals the watch.\
The watch is automatically enabled.

Rewriting the same value into memory as was there does not count as a change.\
When reported, the pidp-1 is halted.

Watches are based on the contents of the address in memory and thus can be used on any memory location that
is modified, including instructions that are modified.

The address is a full 16-bit address.

## File switching

These commands or operations can change the current file, either explicitly or implicitly:

- file, which see
- list, which see
- breakpoint hit
- watch hit
- step

For breakpoints, watches, and steps the source corresponding to the address they have is listed if available.
First, an attempt to find the address in the current file is tried.
If that succeeds, it is used.

If the address does not exist in the current file, then the first file in the file list that contains the address
is used and that file becomes the current file.

If the same address exists in multiple files this can be disambiguated by setting the current file
via the *file* command or by using a file specifier in the *list* command.
