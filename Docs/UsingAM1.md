# Using the **am1** macro assembler for the PDP-1

This document describes the **am1** macro assembler and how to use it.

This is version 3.2 and covers up through am1 version 5.2; it will be updated as needed.\
Edit date 24-Oct-2026\
Update for constant pool changes, mactoam1 changes.

## What is **am1**?

(Yet) Another Macro assembler for the PDP-1.

Why do we need another one?
Simply because the available ones seem to essentially duplicate the old **macro1** assembler with minor tweaks.
It's time for a modern assembler that brings productive features and better error checking and better readability.

Am1 provides some significant new features while maintaining some compatibility with existing macro sources.
Additionally, a separate conversion tool, **mactoam1**, is provided to convert an existing **macro1** source file
to **am1** source that assembles to the same binary.

It can, in fact, produce as its output valid source that can be assembled by **macro1**.

## The features and differences

First, two different assembly modes are supported, generating of **macro1** source and direct generation of rim-loadable
binary code.
While most of the language constructs apply to both, no extended memory features such as *bank* and
*bank references*, are supported by **macro1**, and neither are the PDP-1D extended instructions.

Code will still be generated but should just be for reference as any code in extended memory will just overlay
code in bank 0, and the PDP-1D and special instructions will give an error unless defined in **macro1**.

Features

- Has its own C preprocessor, which does what **cpp** does plus a few additions, see *The preprocessor*
- Provides local variables and local scoping
- Provides a new, explicit, variable support with initializers
- Allows easy use of ascii characters and strings
- Supports octal, decimal, and hex numbers without special statements
- Adds operators for and, or, xor, complement, multiply, divide, mod in expressions
- Adds the need-completion flag, C
- Adds sdb for Type 33 Symbol Generator support
- Adds dpyc, shorthand for *dpy-i C*
- Adds PDP-1D instructions sni, szi, cmi, lai, lia, lsw, swp, scf, sci, ifi, iif, ida
- Can select between keeping -0 or automatically converting to +0 in expressions
- Allows space-separated symbols to be treated as *a | b | c* instead of *a + b + c*
- Adds parenthesized expressions
- Supports extended memory
- Supports drum overlays, parts of a program that take turns in the same core, see *UsingOverlays.md*
- Provides symbol exports and imports for linking programs together
- Can share location symbols across memory banks
- Can generate either **macro1** source or rim format binary code output
- The rim format loads a new loader to support extended memory use
- Does **not** allow redefining symbols (but preprocessor defines can)
- Does **not** treat tab as a statement delimiter
- 'Punches' the readable program header line in binary mode, like the original **macro** does

Differences in source code

These changes were done to allow the above changes and to remove some of the ambiguous
use of characters in **macro1** that were poorly handled.

- Constants are now of the form *[xxx* and *[xxx]*, not *(xxx* and *(xxx)*
- Multiple statements on a line are separated by *;* not tab
- Tabs are not statement delimiters and are treated as a space
- C preprocessor directives of the form *#xxx* are supported
- The operators *& | ~ ^ \* \/ %* (% is mod), as well as *(expr)* are added
- *C* is defined as 04000 for use with IOTs
- *dpyc* is defined as 724007 for convenience
- *sdb* is defined as 722007 for Type 33 support, equivalent to dpy-i if Type 33 not configured
- Directives special to **am1**, such as *%%optimize*, are spelled with a leading *%%*
- The numeric prefixes 0x, 0d, 0o are added to mean hex, decimal, octal regardless of the current radix
- *'x'* is allowed to mean the value of a single ascii character, the usual escapes are recognized
- *ascii "xxx"* is provided to create a table of ascii character values similar to *text*
- The directives *local*, *addlocal*, and *endlocal* are provided to control local scoping
- The directive *bank* is provided to control placement in extended memory
- Location symbols in one bank can be referenced from another bank by using the *:banknum* suffix
- Explicit variable declarations with optional initializers is added, *var name=expr, name...*
- Tables with optional initializers are added, *table 10, 7+5*
- Macro1 style define-terminate blocks are **not** supported, use #defines

## A note on **macro** vs **macro1**

No particular effort is made to remain compatible with the original native **macro**, although if symbol lengths
are limited to be unique in the first 3 characters and operators added in **macro1** aren't used,
the generated code should be compatible.

Compatibility with **macro1** is provided as long as symbols are unique within the first 6 characters.

A **note** about **macro1** *define* and *terminate* constructs - as noted, these are not supported.
But, see next.

## But what about existing **macro1** sources?

Never fear, help is here! - *Dudley Do-right*

The **mactoam1** utility, see its documentation, converts existing **macro1** source code to **am1** source
that assembles to the idential binary **macro1** assembles to, including converting **macro1** defines into **am1**'s
block *#define* ... *#enddef*.
What it cannot convert it refuses giving an *#error* line in the output.

If you have only a binary image, then use the **disassembler** utility in its macro source mode
to create a macro source file you can then process as above.

And of course, you can go back to macro sources from **am1** by using the macro source output flag.

## Flex, flexo, FIO-DEC, concise???

While the native character set used in the PDP-1 is frequently called *flex* or *flexo*, it really isn't.
Flexo is FIO-DEC, a 6 bit character code with an 8th bit of parity.
But what is really used is *concise*, which is just FIO-DEC without the parity bit, resulting in a strictly
6 bit character.

You will see all these variations of naming used in PDP-1 documentation, but just remember, it's all just
6 bit *concise*. Unless it isn't, just to keep things confusing.

In this document, no matter what it's called, it's *concise*.

## 1's complement and math operations

The PDP-1 used 1's complement math, which no modern computers use.
But, **am1** runs on a modern computer and uses 2's complement math.
It automatically converts 1's complement to 2's complement for the math operations
*+, -, \*, /, %*, and unary minus, then converts the result back to the 1's complement
representation after computation.\
The values in the binary output will be the correct 1's complement values.

Note that one annoying thing about 1's complement is that there are two values for zero, 0 and -0.
0 is all bits off, -0 is all bits on, and it is normally automatically converted to 0,
but for the result of math operations only.\
This is the same behavior as the original PDP-1's hardware.

For example, the operation *-1 + 1* would result in -0, not 0 as you would normally expect, so it is converted
to 0.

This can be overridden by a command line flag, but that is not recommended.
It should be used with care, and generally not used at all, since math results are not likely
to be what is expected.
It is really provided more for completeness, not functionality.

Again, this only applies to math operations.
Bitwise operations are not math operations, so for example *~0*
will give the expected 0777777 value, -0.

## What goes on inside?

Unlike other PDP-1 assemblers, this is built with modern(ish) tools.
It uses lex and yacc (flex and bison) for the lexical analysis and parsing.
The result of that is a parse tree representing the program.

Having a parse tree means it's easy to plug in different code generators. Just walk the tree and emit what you want.

Locals are implemented with a local symbol table that is created when a local scope begins.
It's pushed on a stack of local tables if a nested local scope is encountered.
Each ending of a scope updates all the scope's local symbols to reflect the value of the pc where they are
allocated and pops back to the next higher context, if any.

All references within the scope are then emitted as pc-relative so no externally visible symbols are created.

Constants are handled specially to allow for constant reuse.
When a constant is encountered, its defining expression is used to create a hash code that is saved
as the key in the constants symbol table.
When every symbol the expression uses is already defined, the key is its value and
the next time the same value is seen the existing entry will be used.
When it uses a symbol not yet defined, the key is which symbol is in place of its value and
if an operator is applied to it, the expression itself.
The entry is shared only with constants written the same way.
`[foo+1]` and `[1+foo]` share one entry; `[foo+1]` and `[foo+2-1]` each get their own
even though both evaluate to the same value.

When a *constants* directive is seen, all constants added since the last instance of the directive are assigned
locations starting from the current location which will then be updated to one past the last constant processed.
However, the actual value is not computed until the end of the program so that forward references can be used.
Then, every constant's expression is checked against the value of the entry it shares.
A mismatch would be a defect in **am1** and it stops with an error giving the constant's line
rather than load a wrong value.

If there are constants that have not been processed when the end of the program is reached, they will be automatically
processed and placed just after the last location used.
This might or might not be what is intended, so explicitly placing them is advised.

Each memory bank has its own context that holds the global symbols and constants, the current
location, etc.
The context is created the first time a bank is specified by the *bank* directive.
The contexts are automatically switched when banks are switched.

A cross-bank reference looks up the given symbol in the global symbol table of the target bank and becomes
the full 16-bit address of that symbol.
If the symbol does not exist currently in the bank, it will be created.
If it is never resolved in that bank, an error will be given at the end of the program.

Three code generators are implemented, one that emits correct macro1 code that can be assembled by it,
and one that emits binary suitable for rim loading, and one for generating listings.
The parser side knows nothing about the details of the code generators, it just manages the symbol tables
and creates the parse tree.

Wrapping the parsing and code generation is control logic that manages the files and optionally
preprocesses the input, as **cpp** would, using **am1**'s own preprocessor.

## Building **am1**

Be sure you have flex and bison installed, then:

Just type make.

## Usage

```
**am1** [-abdmMlnNrsSTvz[xykp]] [-O[1|2]] [-O=modifier]... [-Dsymbol]...  [-W|-W=name...]
    [-Ipath]... [-ipath] sourcefile
```

- a space means *add*, default is *or*
- b generate binary tape image code, the default action
- d generate all files needed for **ad1**, the same as giving the *-s* and *-l* flags.
  For a program with overlays, the *.osym* file is also generated.
- m generate **macro1** code
- M already used memory overwritten by code is a warning, not a fatal error
- l generate a program listing
- n don't preprocess the input
- N don't keep any text from an included file in a listing
- O run the optimizer advisor, see *UsingTheAm1Optimizer.md*
- O=modifier as -O, with additonal behavior
- O1 as -O but also rewrites code with optimize-enabled regions
- O2 as -O1 with no region needed
- r don't output an initial rim loader
- s generate a symbol table, automatic if non-overlay exports are done
- S print a list of the highest address used in each bank and for a program with overlays,
  where each overlay is on the drum.
  The highest address is the bank's highest word assembled into or reserved by a *table*
  and *none* for a bank with no usage
- T generate test mode output, see below
- v print the version number and exit
- z preserve -0 for math operation results, the default is to convert to 0
- Dsymbol define a symbol for the preprocessor, -Dsym, -Dsym=value or -D sym are all accepted
- W print all warnings
- W=[-]name enable or -disable just this warning, can be repeated for more warnings
- Ipath add a directory to the #include search list, -Ipath or -I path are both accepted
- ipath set the root directory for <file> searches, -ipath or -i path are both accepted

These additional flags are generally for debugging:

- x send lex debugging output to stderr
- y send yacc debugging output to stderr
- k keep the preprocessed source, *file*.cpp
- p print the internal parse tree in readable form on stdout

Both **macro1** and binary code can be generated at the same time.

If warnings are disabled, errors will still be printed.

Both **macro** and **macro1** treat *a space b* as *a + b*, which leads to some tortuous expressions
in code when multiple operands are combined.
By default, **am1** treats a space as an *or* operation, which makes much more sense.
However, the original behavior can be used via the *-a* flag.

If the 'system' include files are not installed in the default location, /opt/pidp1-mods/Am1Includes, then
the location should be specified either by using the *-i incroot* switch or by setting the environment
variable *AM1INCDIR* to the full path to the location.

The -i flag has priority, followed by the environment variable, followed by the default.

**am1** preprocesses its source itself: no **cpp** is needed at runtime.

## The fastload utility

Also provided is *fastload*, a utility to instantly load a rim tape.
If you get tired of waiting, this is the answer, especially for very large programs.

It can load traditional macro rim tapes or am1 rim tapes, with extended memory support.

It supports 2 loading options, directly into running memory or into the memory file read when
the pidp-1 is started.

For direct loading, the pidp-1 must be running with its debugger port enabled.
That is the *ad1port* setting in the pidp1.config file, which is on by default.
The pidp-1 is stopped if it is running, the whole tape is checked and written in one step (a bad tape
changes nothing), and *fastload* then offers to start the program.
It can be on another machine, see *-h* below.

For loading into the memory file, the pidp-1 process must not be running.\
If it is, the memory file will be overwritten.\
You will be asked to confirm it is not running before loading occurs.\
Load with -m first, then start the pidp-1.\
A load that fails, a bad tape or a drum that can't be written, leaves the memory file as it was.

A program with drum overlays, a tape with a *.ovl* beside it (the tape's name with *.ovl* in place of
its extension), has its overlays written to the drum first, as *ovlload* does; see *UsingOverlays.md*.
The *.ovl* is checked whole and the tape's build stamp must be the *.ovl*'s, or nothing is written.
Then the pidp-1 is stopped if it is running, each overlay is written to its track in the drum file,
one line says which overlays and tracks, and the tape is loaded either way above.
A track that held another tool's data, a program *drumupdater* loaded, Adventure's saved game or part of
a Z-machine story, is overwritten and named.

Usage: fastload [-h host[:port]] [-y] [-m] [-f memfilename] [-i drumfile] *rimtapefile*

The *-h* option names a pidp-1 on another machine, as for **ad1**. The default is *localhost:1044*.
A host other than this machine with no port gets 1045, the usual *ad1remoteport*.
Only one debugger client can be connected to a pidp-1 at a time, so *fastload* will refuse if
**ad1** is connected.
With *-h* naming another machine, a program's drum overlays are not written, since the drum file is on
that machine; *fastload* says so and loads the tape, and *ovlload* must be run there.

The *-y* option starts the program once it is loaded, without asking.

The *-i* option names the drum file a program's overlays are written to.
The default is */opt/pidp1-mods/pdp23drum*, the pidp-1's drum.

It will report the starting address and offer to start the program at that address.

## Test mode

Test mode is a special mode used to create output suitable for validation tests of **am1** itself.

When used, it overrides generation of macro, binary, list, and symtab files and enables preprocessing.
A text file named *progname.dmp* is created that has one line of text for every storage word
that has been generated, basically a loaderless *rim* tape but in ascii.

Each line consists of two 18-bit octal numbers separated by a space.
For every 18 bit word that would be loaded into memory its memory address and its value is written.

A word of a drum overlay has the overlay's name after the two numbers since overlays share addresses,
see *UsingOverlays.md*.

The last line is the value of the *start* or *stop* directive at the end of the program.
While technically not a storage word, it is needed for validation.
Its address and value will both be whatever was specified in the statement in the source program.

The resulting file can be used to compare an assembled program's binary representation to a reference copy.

## Warnings and errors

If enabled, various warnings can be printed.\
Some are repeated, some only print once.
If not stated, the warning is only issued once.

The warnings are:
- 1Dop, a PDP-1D instruction, lia, lai, lsw, swp, sni, szi, or cmi was used, repeats
- bank, a bank statement is used but generating macro code (once per bank directive)
- banks, bank usage detected generating macro code, or a cross-bank forward reference was created
- bref, a symbol with a bank reference is used but generating macro code
- stop, a stop statement is used but generating macro code
- locals, a local symbol hides a global symbol or an endlocal does not match the scoping depth, repeats
- flex, a flexo op is used but shift codes make it exceed 3 characters, repeats
- vars, a variables statement was used but there are no variables to emit, repeats
- memory, an expression's address is the same as one that already has had code written to it, but see below
- law, a law -n or law out of the range law i 07777 to law 07777 was used, repeats and enabled by default
- overlay, a var in a read-only drum overlay whose value resets at every load,
  repeats and enabled by default, see *UsingOverlays.md*

Examples:
```
-W=1Dop
-W=bank -W=locals
-W
-W=-flex
```

The first form enables all warnings, but individual ones can be then disabled.\
Prefacing the warning name with a minus turns off the warning, useful with -W.

Errors are always printed and cause assembly to stop.
There are 4 kinds of errors:
- internal errors, these should never happen and if they do, it's a bug
- symbol definition errors occur if a symbol is used but never defined
- syntax errors occur if the program is not structured correctly
- memory violations

The first two will be obvious, the third covers a variety of errors.
These are invalid code syntax, e.g. *1++2*, unterminated text or ascii strings, and number base violations,
e.g. using 9 when the base is octal.

For the fourth, **am1** checks for multiple kinds of memory violations.
These are only checked if binary, executable code is being produced.

- if code tries to go past the end of its bank, i.e. code at an address greater than 07777,
a fatal error will occur.
- when generating binary code, but not macro code, if code or data is placed in a location that already
contains code or data, an *overwrite*, a fatal error or warning will occur.
- when emitting a *table*, *text*, or *ascii* directive, a fatal error on an overwrite will occur.
- when emitting *variables* or *constants* either explicitly or implicitly, a fatal error on an overwrite will occur.

The second, overwrite by code, can be made a warning via the *-M* flag.
By default, it is a fatal error.

Every word **am1** writes to the tape is checked, in both directions: a word cannot land on an address
already used, and a later word cannot land on it.
The words the author writes -- an expression, an instruction or expression on a label's line,
and each element of a *table* with an initializer -- are the ones *-M* covers.
*text*, *ascii*, *variables* and *constants* are always fatal, with or without *-M*.

Note that *-M* only changes an overwrite from an error to a warning; the warning itself is printed only if
warnings are enabled, with *-w* or *-wmemory*.
Without them, *-M* lets the overwrite happen in silence.

## Listing file

The listing file is more complex than the **macro1** version in order to list code that is **#include**ed.
Each file being processed will have an initial line identifying the file the listing that follows is from.

Multiple statements on a line will be listed on separate lines, but the line number will be the same for all.

Empty lines are generally eliminated for clarity, since preprocessing leaves a lot of them where its
directives were.

#define statements are not listed, but the expansion is.

While a listing can be produced for either macro or binary, the binary value for each location is only
available if binary has been generated.
For macro only output, since it is still a source file, no binary value for a location is available.
Of course, **macro1** will produce its own listing file, so creating one via **am1** is fairly useless other
than seeing macro expansions.

The *-N* command line flag can be used to reduce clutter by suppressing any text that comes from an included file.
However, you will not see any information from those. The code in them will still be assembled.

Each drum overlay is listed under its own heading and the drum layout is listed above the overlay table,
see *UsingOverlays.md*.

## Output files

All output files have the original file's name but with a different extension.
The following can be produced:

- *file*.rim - binary output that can be read-in loaded
- *file*.bin - binary output that has no rim loader
- *file*.mac - text output that can (usually) be assembled by **macro1**
- *file*.lst - text output that is a listing of the assembled program, only generated for binary mode
- *file*.cpp - text output that is the preprocessed source, kept when *-k* is given
- *file*.sym - text output that is a listing of the global symbols in the program
- *file*.ovl - text output that holds a program's drum overlays
- *file*.osym - text output that lists the drum overlay symbols, written with the *sym* file
- *file*.map - text output that says where each word came from, through any macros, written with the *lst* file

The *ovl* and *osym* files are written only for a program with overlays, *UsingOverlays.md* describes both.
A program's *sym* file holds the root's symbols only, never an overlay symbol.

The sym file contains 3 initial lines, a label, a version, and the filename of the original file
followed by one line per symbol of the form:
```
%%am1 symtab file%%
Vnnn
aaaaaa F symbol-name lineno
```

where *aaaaaa* is the full 16-bit address of the symbol's location in memory, and *F* is a flag, either *G*, *I*,
or *X*.\
A *G* means the symbol wasn't exported, *X* means the symbol was, *I* symbols were imported.\
The *lineno* value is the line number in the original source file where the symbol was *resolved*, not declared.

### The origin map

Whenever a listing is written (*-l* or *-d*), the origin map *file*.map is written also.
The listing shows a macro's expansion at the line where the macro was used.
The map says which macros made each word and from which line of which file.
**ad1** reads it to show the lines you wrote, see *UsingAD1.md*.
The *lst* and *sym* files are the same with or without it.

It is plain text, one record per line, the fields separated by one space:
```
%%am1 origin map%%
V1
source-file
F n file
W aaaaaa overlay n line [macro n line]...
P aaaaaa
S symbol-name n
```

- The first three lines are a label, a version, and the source file as it was given to **am1**.
- An *F* record numbers a file the first time a record needs it.
  Files are named as the preprocessor found them, relative to the directory **am1** ran
  in or from the root of the include files.
- There is one *W* or *P* record for each word the listing shows, in the listing's order, so the two can be
  paired by counting.
  A listing line can hold two words: a label's, and the first word of a *type340* string on the same line.
- A *W* record is a word assembled from code: its address (bank and address, as in the listing), the drum
  overlay it is in, or *-* for the root, and the file number and line the listing gives its statement.
  Then, when macros created the statement, one triple per macro, from the one the program used inward: the macro's
  name, the file it was defined in, and the line of its body the statement came from.
  A block macro's statements each have their own body line; a one-line macro's all have its one line.
- A statement made from a macro's argument has the origin of where the argument was written, not the macro's.
  A token made by *##* is the macro's.
- A *P* record is a word of a constants or variables pool, which has no origin.
- An *S* record gives the file each symbol of the *sym* file is defined in, in the same order.
- A file number of *-1* means the file is not known.

For example, with *twice* defined on line 2 of *sub/defs.ah* and the block macro *three* on lines 8 to 10 of
*main.am1*, `three(y)` on line 16 gives:
```
F 0 main.am1
F 1 sub/defs.ah
W 000102 - 0 16 three 0 9
W 000103 - 0 16 three 0 10 twice 1 2
W 000104 - 0 16 three 0 10 twice 1 2
```

## Files with no loader

When binary code is being output and the *-r* command line flag is used, no initial rim loader will be
written.
This is useful for creating tapes that will be loaded after a program and is used in conjunction with the *stop*
directive.

Additional tapes loaded must have been generated with the *-r* flag; if not, the results will be undefined.

Common uses are loading tables of data or loading additional already assembled code.

When used, a program that needs to load additional data or code should end with a *stop*.
A no-loader tape can then be mounted and the *continue* switch pressed to load the tape.
As long as tapes end with a *stop*, the process can be repeated.

If the program is to automatically start, the last tape should end with a *start* statement with the
appropriate start address.

## Using macros

Macros are handled by **am1**'s preprocessor, which works as the C preprocessor does.
What it adds and what it does not support are in *The preprocessor* below.

The simple case:
```
#define foo bar
```
is essentially identical to the **macro1** *foo=bar*.

However, defining the equivalent of a macro is different. The preprocessor expects the complete definition
to be one logical line or in a block (below).
One logical line is made by using line continuations and semicolons:
```
#define mymacro(a,b) lio a; cma; lac b

or

#define mymacro(a,b) \
    lio a; \
    cma; \
    lac b
```

The backslash at the end of the line says 'ignore the end of the line, treat the next line as part of this line'.
There is one non-obvious side-effect of this. Consider:
```
#define mymacro(a,b) \ // This is my macro
    lio a; \
    etc.
```

This will fail completely because the comment hides the backslash line continuation.
What this results in is:
```
#define mymacro(a,b) \
    lio a; \
    etc.
```

The \\ no longer means line continuation and is just passed as-is.
So, put your comments before the continuation:

```
#define mymacro(a,b) // This is my macro \
    lio a; \
    etc.
```

A macro of several lines can instead be written as a block with no backslashes or semicolons.
A function-like *#define* with nothing after its parameter list opens the block, *#enddef* closes it.
Each line in between is a statement line of its own, comments can go anywhere:
```
#define mymacro(a,b)    // This is my macro
    lio a               // a comment here is fine
    cma
    lac b
#enddef
```

A block with no parameters is written *mymacro()* and used as *mymacro()*.
The rules are in *The preprocessor* below.

Finally, note that the preprocessor removes comments from #defines when they are expanded in code.

## The preprocessor

**am1** preprocesses its source using its own builtin preprocessor.
It is generally the same as what the traditional C preprocessor, cpp, supports.
It intentionally does not support some of the later extensions to cpp.
- *#define*, *#undef*, *#include*, *#if*, *#ifdef*, *#ifndef*, *#elif*, *#else*, *#endif*, *#error*,
  *#warning*, *defined* and *##* pasting work as in **cpp**
- comments are kept so the listing shows them
- nothing is expanded inside *"..."* or inside a *'...'* pair closed on the same line, so *text '...'*
  strings are safe
- a line starting with *#* that is not a directive is passed through as text

The only name defined at the start is **AM1** (unless *-m* is given) and those given by *-D*.
The host's names that **cpp** defines, such as *linux*, *unix* or *\_\_GNUC\_\_*, are not.

An *#include "file"* is looked for in the including file's directory, then in each *-I* directory
in the order given, then in the include root (*-i*, *AM1INCDIR*, or the default).
An *#include <file>* is looked for in the *-I* directories, then in the include root.
A file found in the include root is a system header, warnings in it are not reported
except those of its own *#warning* lines.

Diagnostics have the form *file:line:column: error: text*, or *warning:* in place of *error:*.
Any error stops the assembly after preprocessing.

### Additions

*#includeonce* has the same forms as *#include* and includes the file unless it is already included.
The file is the first one found on the search path, however its name is what is matchd, *"x.ah"* and
*"./x.ah"* are the same file.
A file brought in by a plain *#include* also counts as included.
```
#includeonce <memory.ah>
```

*#uninclude* undoes the inclusion of a file.
Every *#define* and *#undef* made by that file and by every file it included,is undone.
All go back to the state before the include.
A name the program has changed again after the include keeps the program's change.
The file and every file it included then count as not included.
A later *#include* or *#includeonce* brings them in again.
*#uninclude* of a file that is not included is a warning and does nothing.
If of a file that is still being read, an error is given.
```
#include "tables.ah"
...
#uninclude "tables.ah"
```

Both *#error* and *#warning* can be the body of a macro.
Nothing is reported where the macro is defined, each *use* of the macro reports the message.
A function macro's arguments are put into the message.
An *#error* stops the assembly, a *#warning* is reported and assembly continues.\
Example:
```
#define dra #error "dra has been disabled, use t340dra and t23dra instead."
```

However, unlike gcc, the #warning or #error string itself is not included, and quotes are removed.
The above would print:
```
error: dra has been disabled, use t340dra and t23dra instead.
```

A function *#define* with nothing after its parameter list opens a block macro, *#enddef* on a
later line closes it.
The body is every line in between, each a statement line of its own,  no *\\* or *;* is needed but is allowed.
```
#define farjda(a)
    eem
    dac i [a]
    jsp i [a+1]
#enddef
```
- In the expansion the lines are joined by *;*, a block expands to exactly what the same macro written on
  one line with *;* would give,and assembles to the same words.
  A line that already ends in *;* gets no second one.
- Blank lines add nothing, and comments are dropped, as for any *#define*.
  A comment can follow the opening *#define* or the *#enddef*.
- Parameters, *##*, and nested macros work as in a one-line *#define*.
  A line ending in *\\* is still joined to the next.
- A block cannot hold a directive. A line in it starting with *#* is an error and the macro is not defined.
  A macro whose body is *#error* or *#warning* keeps the one-line form.
- *#enddef* with no block open is an error and tokens after an *#enddef* on the same line give a warning.
  A block not closed by the end of its file is an error, reported at its *#define*.
- The lines after a block keep their own line numbers.
  The listing shows a block's statements at the line where it is used, one listing line each.
- A block and a one-line *#define* that expand to the same tokens are the same definition, so neither
  redefining the other is a warning.
- *#undef*, *#uninclude* and *defined* treat a block like any other macro.

### Not supported

Any of these is an error:
- the *#* stringizing operator
- variadic macros: ... and \_\_VA\_ARGS\_\_
- \_\_FILE\_\_
- \_\_LINE\_\_
- pragma and \_Pragma
- #line
- #include_next
- #import
- #ident
- #sccs
- #assert
- #unassert
- #elifdef
- #elifndef

## PDP-1D instructions

A number of the PDP-1D extended instructions are supported.\
However, the target PDP-1 must support any that are used.\
All are supported by the pidp1-mods PDP-1.

| mnemonic | operation |
|----------|-----------|
|lia | Load IO from AC |
|lai | Load AC from IO |
|lsw | lia lai, swap AC and IO|
|swp | same as lsw |
|cmi | 1's complement IO |
|sni | skip on nonzero IO |
|szi | skip on zero IO |
|scf | clear program flags |
|sci | clear IO |
|ifi | program flags = pf bitwise-or with IO |
|iif | IO = IO bitwise-or with pf |
|ida | index AC, AC = AC + 1 |

## General program structure and comments

All programs start with a title line. This is mandatory. If you forget it, then whatever the first line is
in the source will be the title, probably not what was expected.

The title is taken exactly as written, on the listing and on the tape: the preprocessor does not
change it, though it changes any other line that holds a defined name (**AM1** is defined unless
*-m* is given).
A first line that is a preprocessor directive is not taken as the title. The title is then whatever
the preprocessor puts first, as in versions before am1 v3.8: a blank line, for a *#define*.

After the title line, any number of program lines are given, which can include standard C preprocessor directives
such as #include, #ifdef, etc.

The assembler has a current location counter. This corresponds to the current memory address being
used. It is initially set to location 4 and is changed as statements are assembled.

Program lines consist of *statements*, each of which is an *expression* or a *directive*,
pseudo-instruction in **macro1** parlance terminated by a new line or a semicolon, ;. 
Using a semicolon allows multiple statements on one line.
A semicolon does not change the current line number but does change the current location counter.
A new line changes both.

Comments are also allowed using the new **C** and **C++** notations:

```
// comment
```
or
```
statement.... // comment
```
or block comments are allowed using the traditional **C** notation:

```
/* comment */
```
and
```
/* multi
   line
   block comment
*/
```

However, this form is *only* allowed at the beginning of a line, not within or at the end of a line,
use the // form for that case.
This is illegal:
```
    cla /* end of line comment */
```

Each statement generally increments the current location by one, but see more below.
Comments and empty lines do not affect the current location.

Finally, the last line must be a *start xxx* statement to tell the loader where to start executing
or a *stop* statement to allow for loading additional tapes before starting a program.

The original **macro1** was not particularly good at reporting errors, and a missing start would generally cause
unintended behavior. This statement is now mandatory, enforced by the assembler.

## Expressions

An *expression* is a sequence of *symbols*, *operators*, and *numbers*.
Expressions are evaluated and the resulting value becomes the 18-bit result.

All the terms in an expression are evaluated at *assembly time* to give an integer value;
*there is no computation done during the execution of an instruction*.

**Read** the section on 1's complement math, above!

Some of the math operations are not supported by **macro1**. These will be computed during assembly and
emitted as numeric values.

## Symbols

A *symbol* is a string of characters composed from an initial upper or lower case
alphabetic character or underscore, followed optionally by any number of alphanumeric characters, digits,
and underscore e.g. *scratch_Location_1*.

Note that while **am1** allows effectively unlimited symbol lengths (1023), **macro** only allows 3
and **macro1** only allows 6.
More precisely, symbols must be unique in the first 3 characters for **macro**, 6 for **macro1**.

Unlike **macro1**, a digit cannot be used as the first character of a symbol.

Symbols are of four general and one special types.

- location symbol e.g. *abc34*, which represents a named location in memory
- local symbol, e.g. *mylocal* named in a *local* directive, which represents a location specifically within a local block
- reserved symbol, e.g. *lac*, generally an opcode
- variable
- constant symbol, e.g [123]

## Origin symbols

An example of origin symbols is:

```
100/
4/ lem
start+10/
```
Origin symbols don't generate any code themselves, the just set the current memory location.
New code will be placed starting at the address before the /.
However, any code-generating statements following the / will of course generate code.

The syntax must be followed, the formal definition is:\
*A slash with no blank before it and a blank after it, at the
end of the first expression of a statement, is an origin and the rest of the line is the next statement.*

**NOTE** the important wording. If you use division, /, it *must* have no blank following it.
Otherwise it will be interpreted as the origin character.
```
foo/5/ cli   valid
foo /5/      valid
foo/5 /      FAIL! A syntax error
foo/5 / cli  FAIL! Evaluates as (foo/5/cli)
```
To avoid all the pain, don't use a divide in an origin expression, it would rarely be useful anyway.

## Location symbols

An example of location symbols is:

```
    dac foo
    jmp bar
    lio zip:3
foo, 0
bar, .....
```
The first three are *references*, the value is the memory location assigned to each.
The second two are *location assignments*. Each is given a memory location, which is the current program location.

The form for *zip* is a *fully-qualified* symbol that results in its full 16 bit address, see the section on
*bank*, below.

It is an error to reference a symbol that never has a location assigned to it, and it is an error to define
a location for the same symbol more than once in the same memory bank.

Note that this is a departure from **macro1** which allows arbitrary redefinition of *any* symbol, a bad idea.

A location symbol is *defined* when its name is first used.
It is *resolved* when it is used as a location.
In the example, the first use of *foo* is a definition, the second is the resolution.
The second form is also called a *location assignment*.

## Local and private symbols

Local symbols are a variation of location symbols.
They are defined between *local* and *endlocal* directives, and are named following the *local* statement.

```
    local a, b, target
    target, iot 31
    a, cla
    .
    .
    b,
    jmp target
    endlocal
```

Additionally, more locals can be defined after the initial *local* directive by using *addlocal*.

```
    local a, b
    .
    .
    addlocal c, d
    ...
```

These act in all respects just as if they were added in the original *local* directive.

Unlike **macro1** where all symbols are global and so all symbols must be unique or they will be
redefined,
local symbols allow code to be written that won't clash with other code.
The local symbol exists only within the local block and nested local blocks.

Local blocks can be nested up to 1024 deep.
A local symbol in an enclosing block can be referenced within a nested block, and regular location symbols
can be used and declared within local blocks.

If a local symbol is defined in a *local* or *addlocal* directive but never referenced, it will be ignored,
no code will be generated for it.

*Private* symbols are a variation of local symbols.\
The only difference is where they are defined and where they can be resolved.

A private symbol can be *declared* in an outer scope, but its *resolution* happens on the *first* use seen in
the defining scope or an inner scope.

If there is an existing local scope, the private symbols are added to it.

If no local scope exists, one is created just as if *local* had been used.
This will only happen at the top level of a program where no local scope will exist.

Why is this useful?
It allows an outer scope to access a symbol in an inner scope while hiding it from scopes outside the
declaring scope.

A common case is peculiar to the PDP-1's ability to modify its own code.
An instruction in a nested scope can be modified by an outer scope, useful for dynamic modifications
of locations.

**IMPORTANT** - understand that these statements just declare a symbol as local or private,
they do *not* allocate any storage or assign addresses.
That still needs to be done just like regular location symbols.
This isn't **C**.

**NOTE** an older syntax, %xxx, has been dropped.

## Symbol exports and imports

A program can export its global symbols for other programs to use. This is useful for allowing separately assembled and loaded programs to interact. The exported symbols are in a *.sym* file, see above.

Symbols from other programs can be imported, making symbols in those programs accessible when loaded.

A program has control over exporting and importing symbols by using the *export* and *import* directives.
See the directives *import* and *export*, below.

## Variables

Variables are actually just a shorthand way of representing a common operation, which is naming a location
in memory.

Variables must be explicitly declared via the *var* directive but then can be used just like any location symbol.
A variable can have an optional initializer.

This:
```
   var a, b=34+c
   dac a
   jmp b
```
and this:
```
   dac a
   jmp b
   ...
a, 0
b, 34+c
```
are exactly equivalent.
Also see the *variables* directive below.
   
Because a variable is itself a label, a name declared by *var* can't be declared again by *var* or be used as a
label in the same bank.
Either is an error.
   
## Constants

Constants are another shorthand, but with special behavior.

Examples of constants are:
```
   lac [123]
   lio [a+456   // a trailing comment
```

Functionally, the above is equivalent to:
```
   lac const1
   lio const2
   .
   .
const1, 123
const2, a+456
```

But, constants are also tracked and kept in a *constants pool*.
Successive uses of the same constant all share one memory location for their value, saving space.
A constant whose symbols are all defined where it appears shares a location with every other constant
of the same value in its pool.
One that has a symbol defined later shares only with constants written the same way,
so two that are spelled differently even if they evaluate to the same value take a location each,
see *What goes on inside?*.

If a constant is the last thing on a line except for a trailing comment, the trailing ] can be omitted.\
*Note* that *last* means **last**, nothing but an optional trailing comment is allowed.\
Best practice is to always close the constant.

Also see the *constants* directive, below.

## Tables

Tables correspond to the **macro1** *dimension* pseudo-op.
A table statement reserves a number of words, optionally initialized to a value.
The table keyword is followed by an expression that must be able to be fully evaluated at that time.

Examples are:
```
table 10
table 5*7
```

An optional expression can follow which will be the value to fill the table with.
If no value is given, the table will not be initialized:
```
table 10, 'z'
```
fills the table with the ascii value of the letter *z*.

The table keyword is a statement, and as such must be on a line by itself.
This means that:
```
foo, table 5
```
is **not** legal.

Functionally, it is equivalent to the same number of the initializer value, e.g.
```
table 5, 17
-or-
17
17
17
17
17
```

Or, if there is no initializer:
```
table 5
-or-
.+5/
```

Using an initializer means that a 'tape' word will be written for each element of the table, which for large tables
can significantly increase the size and load time.

## Operators

The following operators are defined and listed in order of increasing priority.\
The priority is the same as that for **C** and the operations are the same.

| Operator | Meaning                |
|----------|------------------------|
| \|       | bitwise or             |
| ^        | bitwise xor            |
| &        | bitwise and            |
| << >>    | left-shift right-shift |
| \+ \-    | addition subtraction   |
| \* / %   | multiply divide modulo |
| ~        | complement             |
| -        | unary minus, -n        |
| ( )      | expression nesting     |

Internally, 2's complement arithmetic is used for most of the operators, but the result is adjusted to be
a 1's complement value.
The 1's complement -0 value, 0777777, can be produced by math operations such as -1+1,
but by default will be converted to +0.
This can be overridden if -0 is to be kept, see *Usage*.

Bitwise operations can of course affect any bit and their result is not adjusted, they are not math.
If a -0 bit pattern results, it is kept as-is.

## Numbers

Numbers are just that, an optional leading *-* followed by sequence of digits.
However, the interpretation of the digits can be either as octal or decimal numbers, depending upon
the current radix, see *octal* and *decimal* below.

Several special representations for numbers are also provided, they override the current radix:

- 0oNNN an octal number, N must be 0-7
- 0dNNN a decimal number, N must be 0-9
- 0xNNN a hexadecimal number, N must be 0-9a-f or A-F
- 0bNNN a binary number, N must be 0-1

There are two additional special number representations:

- char cl, cm, or cr
- 'c'

The first is the same as the **macro1** version, *c* must be a valid *unshifted* Flex/Concise character, and the lmr
specifies which 6 bit field in the resulting 18 bit value the character's value is placed.

The second results in the value of the ascii character, the usual escapes are allowed:

- \\t tab
- \\e escape
- \\b backspace
- \\^c control character c
- \\f formfeed
- \\n newline
- \\NNN the value represented by the 3 **octal** digits
- \\\\ backslash

Even though the above are interpreted in the base they specify, when emitted in the output, they will be
given as the equivalent value in the current radix, octal or decimal.
For example, if the current radix is octal, then 0x1f will be emitted as 37, the octal equivalent.

## Directives

Directives, aka pseudo-instructions, tell the assembler to do special operations.
Some generate code, some don't.
In the list below, letters inside parentheses, (), are optional.

Directives are:
- location assignment, xxx/
- loc(al)
- addloc(al)
- endloc(al)
- octal
- decimal
- flexo
- text
- ascii
- var
- variables
- constants
- bank
- %%optimize
- %%endoptimize
- %%drumreserve
- %%window
- %%overlay
- %%endoverlay
- %%ovcall
- %%ovload
- %%ovtable
- start
- stop
- import
- export
- two special directives, see below

## Location assignment 

*Location assignments* directly set the current location to the value given, which must be an expression
that evaluates *at that time* to a value. This means that for any use of a symbolic location, that
location must already be resolved.
Using a forward reference actually makes no sense, and **macro1** doesn't allow it either.

**This statement must be on a line by itself**, optionally followed by a comment.
This is to disambiguate it from an expression with a division operator.
If you do want to have code on the same line, use a semicolon separator.

Examples are:
```
100/    the location counter is set to 100
.+10/   the location counter is incremented by 10

a, foo
a+10/   the location counter is set to the location of foo plus 10, a must be defined *prior* to its use

200/; cla
```

## Octal and decimal

These two set the current radix for numbers entered without an explicit radix specification.
They stay in effect until the next one is seen.
Any 'bare' number will be interpreted in that radix and an error given if the digits 8 or 9 are used in octal mode.

Examples are:
```
decimal
law 9   results in the octal word 700020

octal
law 9   will give an error
```

Regardless of the radix, the proper binary representation results.

## Flexo

This packs up to 3 flex/concise characters into an 18-bit word.
The first character is in the high 6 bits, the next in the middle 6 bits, the last in the low 6 bits.
If not enough characters are given to fill the word, the space character, octal 0, is used.

**NOTE:** Remember that upper and lower case characters require a shift character, so something like
```
flexo Ok?
```
will not do what you expect.
That sequence actually requires 6 characters, uppershift, O, lowershift, k, uppershift, ?.
Unlike **macro1**, this is detected and a warning given.

Flexo supports the same 3 special sequences as *Text*, following.\
Remember that only 3 characters can be used, each sequence is a character.

## Text

The **text** directive inserts a block of flex/concise characters packed 3 to a word.
The location counter is incremented by the number of characters divided by 3, plus 1 if the number of characters
wasn't a multiple of 3.

The string of characters is surrounded by a marker character, which can be any valid ascii character.
The enclosed ascii characters are converted to flex/concise characters automatically.

Examples:
```
   text "this is text"
   text !This is text!
```
The first results in 4 words, 3 characters per word, packed first in the high 6 bits, second in the middle 6 bits,
third in the low 6 bits. The location counter is advanced by 4.

The second results in 5 words because of the need to insert an upper-shift character and a lower-shift character.
The location counter is advanced by 5.

Unlike the *ascii* directive, there is no end marker. You must know in your program how many words were used.
Safe practice would be to put a marker word following the text with a value to indicate no more words.
The flex/concise stop code, 013, is a good choice.

Additionally, 3 special sequences are supported.
These are introduced by a backslash, \\.
The sequences are:

| sequence | effect |
|----------|--------|
| \\B      | insert a ribbon black-shift code, 034 |
| \\R      | insert a ribbon red-shift code, 035 |
| \\n      | insert a carriage-return code, 077 |
| \\\\     | an actual backslash, equivalent to shifted 002 |

Example:
```
   text "A \Rred\B word\n"
```

## Ascii

The **ascii** directive inserts a block of ascii characters packed 2 to a word and always terminated by a binary 0
byte, as **C** strings are.
The first character is in the high 9 bits of the word, the second in the low 9 bits.
In each case, the high bit is not used and will always be 0.

The location counter is advanced by the number of characters divided by 2. If there are an even number of characters,
one additional word is written and the location advanced by one more.

The string of characters is always surrounded by double-quotes, ", and the same escape sequences supported by
the **C** number form are supported.

Examples:
```
   ascii "this is text"
   ascii "this is text!"
```
The first results in 7 words, 6 for the 12 characters and 1 for the terminating binary 0.

The second also results in 7 words, 6 plus the high 9 bits in the 7th word for the characters with
the remainder of the 7th word binary 0.

In both cases the location counter is advanced by 7.

## Type340

The **type340** directive inserts a block of characters from the Type 340 Display character sets
 packed 3 to a word.
The location counter is incremented by the number of characters divided by 3, plus 1 if the number of characters
wasn't a multiple of 3.

The string of characters is always surrounded by double-quotes, ", and some escape sequences supported by
the Type 340 are provided.

Since it is mandatory for a Type 340 character string to end with an *end* character, one is automatically
added if it is not provided. Note that placing one in a string immediately ends the string, any following
characters will be ignored.

Just as for *flex/concise* characters, there are modal shift characters.
The shift state last given stays in effect until a new one is seen.
At the beginning of a string, the mode will always be upper-shift.

As a convenience, shift characters are automatically added based upon the character.
The actual case displayed depends upon the shift state and the character sets that are enabled.

Various special escape sequences can be used inside the string:

| Escape sequence | Meaning |
|-----------------|-----------|
| \\e | end |
| \\U | upper shift+ |
| \\L | lower shift+ |
| \\A | automatic shift+ |
| \\b | backspace* |
| \\n | newline, a carriage return** |
| \\l | linefeed |
| \\r | carriage return** |
| \\s | superscript* |
| \\u | subscript* |
| \\00 | 1 or 2 octal digits 0-7 |

The characters marked with an asterisk, *, are only valid when character set 2 is in use.\
If octal digits are given, a character of that octal value is inserted.

** - newline maps to carriage return because the Type 340 treats a cr as cr-lf internally.

For the shift escapes, marked with a +, explicitly shifting disables automatic shifting
until the command completes or until automatic shifting is enabled again.

See the *UsingType340Display.md* document for the characters and more information.

Examples:
```
   type340 "t\Lhis is TEXT for the \UT\Lype 340 \UD\Lisplay"
```

This will, assuming the second character set is enabled, display the string *This is text for the Type 340 Display*.

## Variables and constants

These cause any declared variables or constants to be emitted at the current location.
Subsequent declarations will be held until the next variables or constants directive.

If no directive is given, any variables and constants will be written at the end of the program, the
location where the start or stop directive is given.
This might not place these where you want, so using the directives is advised.

The location counter is updated appropriately after either directive.

## Bank and cross-bank references

The bank directive causes all following code to be placed in the bank given, which must be between 0-15 decimal,
0-17 octal.

Each bank has its own global symbols and constants. These are preserved for each bank, so if a subsequent
bank command switches back to an earlier bank, all of its symbols and constants will still be valid.
The current location in that bank is also saved, and will be the same as it was when a bank switch was done.

Initially, the bank is 0 and the current location is 4 to match the behavior of **macro1**.\
The first time a bank is switched to, **the current location will be set to 0** in that bank.\
Afterwards, the value of the current location is preserved for each bank across bank switches.

Remember to use the *constants* and *variables* directives in *each* bank where constants or variables are used.

Global location symbols in one bank can be referenced from another bank by using a *bank reference* modifier on
a location symbol, *sym:bankno*, *sym:.*, or *sym:\**, to reference a symbol.

When it is used, the value is the 16-bit address of that symbol.

The *sym:.* form means *in this current bank* and results in a full 16-bit address for the symbol.

For the *sym:bankno* form, if the location symbol is not defined in the target bank, it will be created.
However, it must be resolved at some point in that bank or an error will be generated.

The *sym:\** form is a wildcarded location. It means *whatever bank that symbol is defined in*.
Since the same symbol name can be defined in multiple banks, a search rule is used.
At the end of parse tree generation the location is resolved by searching the list of banks used from
the first declared in a program by a *bank* statement to the last declared.
The first match is the resolution.

However, if it is defined in the bank in which it is used, the symbol in that bank will be used and
no searching done.

If not found in any bank, an error results.

For example:
```
100/
eem

bank 1
200/
lac a
a, 0
lio i [a:.]
lac b:*

bank 0
b, lac a:1
   lac a:*
```

In bank 0, a:1 will have the value 10201, as will a:\*.\
In bank 1, a will have the value 201, its in-bank address, while a:. (a:dot) will have the value 10201.

In bank 0, b will have the value 100.\
In bank 1, b:\* will have the value 00100, since the full address of a bank 0 symbol is the same.

To reinforce, a : always results in a 16-bit address regardless of where it is used.\
The formal calculation is *(bankno << 12) + symbol-value*.

As a convenience, the form *integer:bankno* can be used.
The same as above, it is a shorthand for *(bankno << 12) + integer*, e.g. 100:3.

Code using banks and cross-bank references must understand the standard PDP-1 extended memory mode access rules.

As an example, the *jda* instruction does not work with extended memory. It uses the indirect bit as part of
the opcode and will not actually indirect.
A macro that implements a working jda for extended memory, *farjda()* is provided in the <memory.ah>
include file along with other macros useful for cross-bank referencing.

**VERY IMPORTANT!**

When a cross-bank call is made (jsp), the address that the target gets is a full 16 bit address.
That means the calling bank number is included.

If you call from an odd-numbered bank and then the routine turns off extended memory, that odd bank will
be set in the low bit of the bank number field.
That happens to be the *indirect bit* when extended memory is off, and your code will not do anything like
you expected.

If you have a routine that is called cross-bank and it turns off eem, then does any indirect references through
any data passed, including the return address, that can have bank bits on, you have to mask them off!

How do you know if eem is on or off?\
Bit 1 (in DEC numbering), next-to-msb, will be 1 if eem is on, else 0.

The **macro** and **macro1** compilers do not support extended memory,
so code using banks will generate code that isn't actually usable, but it will be annotated
to show where banks were switched.

## %%optimize and %%endoptimize

These two directives mark out a span of code that you are willing to have
**am1** rewrite under `-O1`. They are the assembler's half of the optimizer;
`Docs/UsingTheAm1Optimizer.md` describes what the advisor and `-O1` do with
them and what the report says.

```
%%optimize
        cla
        lac count
%%endoptimize
```

**They generate no code and they cost nothing.** No word is emitted, the
current location does not move, and the line does not appear in the listing, in
the macro1 source or in the binary tape. A source with the directives in it
assembles to output byte for byte identical to the same source without them.
They are markers in the parse tree and nothing else. That holds with and
without `-O`; under `-O1` the words *inside* a region may be rewritten, which
is what the region is for, but the directive lines still contribute nothing.

What they mean is a statement *by you* about the code between them:

- no word of it is modified while the program runs,
- no word's address is used as a value,
- and none of it is there to take the time it takes.

That is the one thing the assembler cannot work out for itself. The optimizer
checks the first two against what it can see and tells you if it disagrees.
The third it cannot check at all.

Four arrangements are errors, each reported with the line it is on:

- an `%%optimize` inside a region that is already open. A region is a flat
  span; it does not nest, and the `%%endoptimize` that followed would be
  ambiguous about which of the two it closed.
- an `%%endoptimize` with no region open. Almost always a deleted `%%optimize`.
- a region still open at the end of the source. Taking it as reaching to the
  end of the file would hand out a permission you did not give.
- a region opened in one file and closed in an included one, or the other way
  about. An include is expanded wherever it is used, so half of such a region
  is text the author of the other half never saw.

The directives take no arguments. There is no way to name a transform or a
level, and that is deliberate: `-O1` is the only thing that acts on a region
so far, and fixing that syntax before there is a second thing to tell apart is
how it gets fixed wrong.

Like every directive special to **am1**, they are spelled with a leading `%%`,
so the plain words `optimize` and `endoptimize` are ordinary names. A `%%`
before a name that is not a directive is a syntax error, and so is a
directive's name with more of a name run on after it (`%%optimizex`): the
next character may be anything but a letter, a digit or an underscore --
usually a blank, a tab, a `;`, a comment or the end of the line.

## Drum overlays

Seven directives let parts of one program share the same core locations.
Overlays are kept on the Type 23 drum and loaded when called.
*UsingOverlays.md* is the full description.

```
%%drumreserve 20                // tracks the overlays may not use
bank 1
%%window work, 2000, 3777       // core the overlays of work take turns in
%%overlay plus, work            // or: %%overlay plus, work, persistent
        export plusRun
plusRun, ...
%%endoverlay
bank 0
100/
#include <DRUM/overlay.ac>      // the manager, before the first %%ovcall
        ...
        %%ovcall plus, plusRun  // load plus if it is not in work, then call plusRun
        ...
%%ovtable                       // the overlay table, labeled ovTable
        start go
```

- *%%drumreserve list* names drum tracks, 0 to 37 octal, that overlays may not use.
- *%%window name, first, last* declares memory in the current bank for the overlays it owns to reside.
  *%%window name, first, last, twopass* makes the swapping of its persistent overlays a write
  and then a read, not one swap.
- *%%overlay name, window* and *%%endoverlay* bracket an overlay, assembled at its window's first
  address plus 2 in the window's bank.
  *export* inside it names the symbols the root can reach, entries or data.
  An overlay's labels are private to it and a symbol it does not define is in its bank's globals.
- *%%ovcall overlay, entry* is the root's call of an overlay's entry.
  It is not allowed inside an overlay and must be on its own line.
- *%%ovload overlay, symbol*, the overlay is made
  resident and AC will have the full address of *symbol* which the overlay exports.
- *%%ovtable* emits the table the manager uses.
  Give it once, after every window and overlay had been specified.

The overlays are written to *file.ovl*, not to the tape.
*-O*, *-O1* and *-O2* will also optimize a program with overlays.
*-d* writes the *.osym* file also for use by **ad1**.

## Thisbank

You can use *thisbank* in any expression.
It always evaluates to the numeric value of the bank base address for the bank in effect
when it is used.

For example:
```
lac [thisbank
```
Lac will be loaded with whatever the current bank is at this point left-shifted by 12 decimal.
This gives the address of location 0 in the bank, e.g. if in bank 1, *thisbank* will have the value 4096 decimal.

## Start or stop

The start or stop directive must be the last statement in a program, and this is enforced.
*Start* tells the loader where to begin running the program.
The start address can be a numeric address or a location symbol.
The start address can also be in any bank and can be a shared location symbol.

The *stop* directive tells the loader to halt instead of starting the program.
Additional tapes can then be loaded via read-in.

**IMPORTANT** - an unqualified start means *in the current bank*:
```
bank 2
start foo
```
will look for *foo* in bank 2 and tell the loader to start there.

If you want to start in another bank, typically bank 0, either qualify the location with a bank or
switch to the desired bank before using the directive.

Examples are:
```
start 100
start begin
start 20100 // start at location 100 in bank 2
start begin:3 // start at the location of *begin* in bank 3
stop
```

## Export and import

The *export* directive is used to indicate which symbols are to be shared with other programs:
```
export symbol, symbol, ....
```

The directive can be repeated as often as desired.
Each symbol listed is marked for export.
This automatically asserts the *-s* command flag if it has not been given.

The symbol is not qualified with a bank reference, its bank is the current bank when the export is done.
Exporting a symbol will define it in the current bank if it isn't already defined,
and as usual it is an error if it is never resolved.
```
bank 3
export a, b, c
.
.
.
bank 1
export x, y, a
```

Note that symbol 'a' exists separately in two different banks.

The *import* directive names a file to import symbols from, and has the same form as the *#include* directive.
It can also be repeated as needed.
```
import "foo.sym"
import <libx.sym>
```

Just as for include files, the bracketed form looks in the system include directory for the file.

The exported symbols in the file will be created in the global symbol table for the bank they were exported from
and will be resolved to the address they were exported with.

It is an error to duplicate a symbol of the same name in the same bank, it is treated as if it were defined
in the bank. This means any import must be done before a symbol is resolved.

Imported symbols follow the same rules as cross-bank references; the address will always be 16 bits except in the
bank where the symbol was defined, in which case it will be 12 bits.

No code is generated for imported symbols, they are just references.
Note that the symbol file will contain all global symbols from the exporting code, only those marked
as exported will be imported.

They are accessed using the normal inter-bank notation, *sym:n*.

## Special directives

There is one special directive that is not of general use.
It is the line and file marker the preprocessor inserts, used to keep the current file name and line
number correct.
It will always be on a line by itself and is of the form:
```
# number "filename"
```

It is recommended that it not be used.

## An example

This is a simple example that shows various aspects of multi-bank operation.

```
Simple am1 demo using multiple banks

// The loader will have left extended memory enabled.
// The initial bank is 0.
#include <memory.ah>

100/
       cla
loop,  lio val; lio val; lio val        // just a delay
       add val
       sma
       jmp loop
       farjda(update:*) // a macro in memory.ah
       lem              // check the macros, some stay in eem state
       jmp loop

val,   1              // initially, we increment

bank 1
100/
update, 0             // we jump here from bank 0
       farenter(rtn)  // handles proper setup, NOTE it turns off eem!
       // So we have to turn it back on again, could have eliminated both,
       // all it does is 'dac rtn; lem', so since we actually want eem,
       // a simple 'dac rtn' would be fine.
       eem
       lac i [val:0   // change the sign of the operand
       cma
       fardac(val:0)  // another macro
       lac update
       farrtn(rtn)    // and back to caller in bank 0
rtn,   0
constants

start 100
```

## Reserved symbols

This is a complete list of all reserved symbols.
Reserved symbols cannot be redefined as location symbols within a program.
However, the preprocessor can redefine them via the *#define* directive, since it runs first.

|Miscellaneous   | Value |
|----------------|-------|
|1s | 01|
|2s | 03|
|3s | 07|
|4s | 017|
|5s | 037|
|6s | 077|
|7s | 0177|
|8s | 0377|
|9s | 0777|
|i | 010000|
|C | 004000|

|Opcodes         | Value |
|----------------|-------|
|and | 0020000|
|ior | 0040000|
|xor | 0060000|
|xct | 0100000|
|cal | 0160000|
|jda | 0170000|
|lac | 0200000|
|lio | 0220000|
|dac | 0240000|
|dap | 0260000|
|dip | 0300000|
|dio | 0320000|
|dzm | 0340000|
|add | 0400000|
|sub | 0420000|
|idx | 0440000|
|isp | 0460000|
|sad | 0500000|
|sas | 0520000|
|mus | 0540000|
|dis | 0560000|
|mul | 0540000|
|div | 0560000|
|jmp | 0600000|
|jsp | 0620000|
|law | 0700000|
|skp | 0640000|
|spi | 0642000|
|szo | 0641000|
|sma | 0640400|
|spa | 0640200|
|sza | 0640100|
|szf | 0640000|
|szs | 0640000|
|ral | 0661000|
|rar | 0671000|
|ril | 0662000|
|rir | 0672000|
|rcl | 0663000|
|rcr | 0673000|
|sal | 0665000|
|sar | 0675000|
|sil | 0666000|
|sir | 0676000|
|scl | 0667000|
|scr | 0677000|
|nop | 0760000|
|opr | 0760000|
|cli | 0764000|
|lat | 0762200|
|cma | 0761000|
|hlt | 0760400|
|cla | 0760200|
|lap | 0760300|
|clf | 0760000|
|stf | 0760010|

|1D instructions | Value |
|----------------|-------|
|lia             | 0760020|
|lai             | 0760040|
|lsw             | 0760060|
|swp             | 0760060|
|cmi             | 0770000|
|sni             | 0644000|
|szi             | 0654000|
|scf             | 0740040|
|sci             | 0740100|
|ifi             | 0742000|
|iif             | 0744000|
|ida             | 0740400|


|Various IOTs    | Value |
|----------------|-------|
|iot | 0720000|
|ioh | 0730000|
|tyi | 0720004|
|rrb | 0720030|
|cks | 0720033|
|rpa | 0730001|
|rpb | 0730002|
|tyo | 0730003|
|ppa | 0730005|
|ppb | 0730006|
|dpy | 0730007|
|dpyc | 0724007|
|sdb | 0722007|
|lem | 0720074|
|eem | 0724074|

|SBS ops         | Value |
|----------------|-------|
|esm | 0720055|
|lsm | 0720054|
|cbs | 0720056|

|SBS16 ops       | Value |
|----------------|-------|
|dsc | 0720050|
|asc | 0720051|
|isb | 0720052|
|cac | 0720053|

If a keyword has parentheses, e.g. loc(al), the part in the parentheses is optional.

| Keywords      |
|---------------|
| %%drumreserve |
| %%endoptimize |
| %%endoverlay  |
| %%forcelocal  |
| %%optimize    |
| %%ovcall      |
| %%ovload      |
| %%overlay     |
| %%ovtable     |
| %%window      |
| addloc(al)  |
| ascii       |
| bank        |
| char        |
| constants   |
| decimal     |
| endloc(al)  |
| export      |
| flexo       |
| import      |
| loc(al)     |
| octal       |
| priv(ate)   |
| start       |
| stop        |
| table       |
| text        |
| thisbank    |
| type340     |
| var         |
| variables   |
