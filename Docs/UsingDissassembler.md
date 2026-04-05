## Using the disassembler for the PDP-1

This document describes the disassembler and how to use it.

This is version 1.1 and covers disassembler version 2.1; it will be updated as needed.

Edit date 5-Apr-2026

## What is the disassembler?

It is a two-pass disassembler for PDP-1 binary image files for which a specific loader has been implemented.

Four modes are supported:

- list the disassembled image in an informative format similar to a listing file
- emit source that can be assembled by **macro_1** but see below
- emit source that can be assembled by **am1**
- just emit the raw data as 18 bit octal words

The difference between macro mode and am1 mode is that macro mode cannot deal with extended memory
programs.

If extended memory use is detected, a warning will be included as a comment in the output.
Such a program cannot actually be assembled by **macro1** without review and editing.

If extended memory is not used, then output from either mode can be assembled by either assembler.

## Loaders

The disassembler supports loaders which are responsible for fetching the addresses and instructions from
the input file.
Any type of input file can be processed if there is a loader for it, including memory dump files.

The loader to use is specified by the *-Lldr* flag, optionally followed by a comma-delimited argument list,
see the mem loader for an example.
If arguments are supplied, they are interpreted by the specific loader.
An empty argument, *,,*, is passed as an empty string, *""*.

New loaders can be easily implemented, although **disassembler** will need to be rebuilt when a new one is added.

Currently implemented are:
- bin, a loader for the common DDT loader
- am1, a loader for **am1** rim tapes with the **am1** loader
- mem, a loader for the memory dump files produced by **pidp-1**
- kahlah, a loader for the unknown loader used by the kahlah program; this is probably a loader that predates DDT

## The bin loader

This loader loads tapes that have digital-1-3-s-mb_ddt.bin as the RIM loader.
This is the most common loader.

## The am1 loader

This loader loads tapes that were created by the **am1** assembler and have its extended loader.\
This loader takes no arguments.

## The kalah loader

This loader loads only one tape known so far, the kalah game from 1961.\
It seems this was a very eary loader, possibly descended from the TX0 computer.\
This loader takes no arguments.

## The mem loader

This loader loads from the memory file produced by the **pidp-1** emulator.\
It takes up to 3 arguments:
```
-Lmem,begin,end,start
```

The *begin* arument is the memory location to begin disassembly from, the *end* is the last location.\
If *begin* is omitted, it defaults to 0.\
If *end* is omitted, it defaults to 07750.\
If both *begin* and *end* are omitted, the default is 0-07750.\
If *start* is given, it is the address to use in the *start nnnn* assembler directive.\
If *start* is omitted, it defaults to the *begin* address.

Note that the addresses are full 16 bit addresses to cover all of extended memory.\
This loader takes no arguments.

## Usage

**disassemble** [-?] [-a|m|r] [-kld] [-L loader[,arg...]] [-o outfile] infile

- ?, list the supported loaders and exit
- a, output in **am1** assembler form with bank support
- m, output in pure macro assember form, warn about extended memory use
- r, raw mode, just dump every binary word as an octal value
- k, output RIM loader code if seen and not in verbose mode; normally no, assemblers usually add it
- l, output the leader in readable form, only in verbose
- d, enable diagnostics for debugging this progam
- L loader, use the named loader with optional loader-specific arguments, the default is the bin loader
- o, output file to use, the default is stdout

Flags can be together, *-mid*, or separate, *-m -i -d*.\
Flags that take an argument can be e.g. *-Lloader,arg* or *-L loader,arg*.

Example:
```
disassemble -m -Lmem,0000,07700 -o foo /opt/pidp1-mods/coremem
```

## Limitations

This program must be used with some care. A binary tape does not specify if a block of information is actually
PDP-1 instructions or if it's data. As an example, **macro1 will produce a block for constants, but not one
for variables.

There is nothing to identify constant blocks as such, that must be decided by the user, and variable
blocks don't exist, but see *Labels*, below.

There is also nothing to indicate what is data and what is instructions.
Data that matches an instruction will be emitted as that instruction.

## How it works

When started and the specific loader being used has not told **disassemble** to not look for a RIM block,
it searches for the pattern that identifies the RIM block.
Normally, loaders that process tapes ignore any tape character that does not have bit 0200 set.
Any non-binary character is ignored, even if in a word sequence.

However, any non-binary characters that are seen up to the first binary character on a tape are considered to be
part of a label for the tape and will be output in the form they would appear if you were looking at a physical tape.

When the initial RIM code has ended or has not been processed, control is then passed to the loader that was specified.
The loader then returns address and data words for each memory location it finds and instruction disassembly
is performed.

Two passes are done. The first pass identifies which memory locations have been specified and determines whether
the instuction at that address references other addresses, such as a *jmp xxxx*.
Those addresses are then marked as used and also marked as to whether they are being read from or being written to
and if they are the target of a *jmp*, *jsp*, *jda*, or *cal*.

After the first pass, the marked locations are then scanned and labels generated for any that were a target as
identified above.

The second pass then runs, rescanning the file, and the decoded instructions with labels are emitted.

## Labels

During pass one, an attempt is made to determine how a location is used.
One or two of a possible three prefixes, **T**, **V**, and **C** followed by a number is assigned
to any location that is referenced by another instruction.

If a location is the **T**arget if a jmp, jsp, jda, or cal instruction, it is labeled with **T**.\
If a location is written to by any memory-modifying instruction, e.g. *dac*, it is labeled as a **V**ariable.\
If a location is only read from, e.g. *lac*, then it is labeled as a **C**onstant.

For the common case of a location that is both a target and modified, as for the return from a jsp or jda, then
it will be labeld with **VT**.

If an instruction is an indirect instruction, then even though it is modifying some target, it is not modifying
its direct target, it counds as a read of the target.

The labels will then be shown as part of the instruction that targets a location and on the location itself.

## Embeddable disassembler

The deconde_instruction.c code provides a standalone instruction disassembler that can be embedded in other code
that needs a readable version of a binary instruction word.
This is the same one used by **disassemble** itself.

## Errors

Errors are reported on stderr and are:

- Incorrect command line arguments
- Can't open input file
- Can't open output file
- Nonstandard tape, no initial RIM block if it is being looked for
- Premature EOF
- A loader reported an error during its processing

An error causes immediate termination with an exit code of 1.

## Building

In Tools/Disassembler, just do:

make

## Implementing a loader

Two files should be consulted, *loader.h* and *loaderdefs.h*.
The first is used by loader code, the second defines a loader to **disassemble**.

A loader is called with a sequence of commands:
- LOADER_CMD_INIT   called once when **disassemble** is started
- LOADER_CMD_START  called each time the input file is to be repeated or a new file is given
- LOADER_CMD_NEXT   called to return the next address and word

A loader returns statuses:
- LOADER_ERROR  an error occured, typically a malformed file or premature EOF
- LOADER_OK a generic acknowledgement, command succeeded but caller should ignore any data returned
- LOADER_AGAIN  the command succeeded, but should be issued again
- LOADER_MORE data is returned and there could be more available
- LOADER_DONE the last data available has been returned
- LOADER_STOP the same as LOADER_DONE, except an **am1** *stop* was seen

*LOADER_START* sets the returned address and data to the address that was seen as the block beginning
address, although a loader can return whatever address it wants.

*LOADER_DONE* sets the returned address and data to the address that an assembler *start nnnn* directive
would use, the locaction to begin execution of the program.

*LOADER_STOP* means the program should not be started, the PDP-1 will be in a halt state if it was actually loaded.

A typical flow would be:
| Issued to loader | Returned by loader |
|------------------|--------------------|
| LOADER_CMD_INIT  | LOADER_INIT_x |
| LOADER_CMD_START | LOADER_OK or LOADER_AGAIN |
| LOADER_CMD_NEXT  | LOADER_MORE, LOADER_AGAIN, LOADER_DONE, LOADER_STOP |

The disassembler will do this cycle twice, once for pass one, once for pass two.
However, the *LOADER_CMD_INIT* will only be issued once when *disassembler* starts.

A loader should return *LOADER_MORE* as long as there is data in a current block of input to read.
If a block boundary changes, *LOADER_AGAIN* is the recommended return status.
The disassembler will continue to until it sees a termination condition in which case it should return
*LOADER_DONE* or *LOADER_STOP*.

Of course, a *LOADER_ERROR* can be returned at any time.

The disassembler will generally ignore *LOADER_OK* and *LOADER_AGAIN* except in the read loop
when it is issuing *LOADER_CMD_NEXT*.
In that case, it will just reissue the command.

Finally, the new loader should be registered in the *loaderdefs.h* file, the use is obvious.

A good example to study is *binloader.c*.

## Author

Bill Ezell (wje)\
Send comments or changes to pidp1@quackers.net

## License

This software may be freely used for any purpose as long as the author credit is kept.
It is strongly asked that the revision history be updated and any changes sent back to pdp1@quackers.net so
the master source can be maintained.
