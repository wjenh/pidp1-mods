# Using the mactoam1 converter

This document describes the mactoam1 converter and how to use it.

This is version 2.0\
Edit date 24-Oct-2026\
New version

## What is mactoam1?

Mactoam1 converts a **macro1** source file to **am1** source that assembles to the same binary words macro1
assembles to, at the same addresses, with the same start address.

Am1 and macro1 differ in meaning in several ways:
- a space between two terms adds in macro1, but ORs in am1
- macro1 evaluates left to right with no precedence, am1 has standard precedence
- `-0` is a value in macro1 but not in am1 by default
- macro1's *define* arguments are values, am1's *#define* parameters are text

Mactoam1 does not do just literal text tranformation.
It runs macro1's own two passes over the source using a port of macro1's assembly core, it sees every
expression, tag, constant and macro use the way macro1 does.
It writes each expression in am1 syntax and then checks it.
The am1 text is evaluated with am1's rules, with the *#define*s and parameters expanded as am1 expands them
at every place macro1 evaluated the original, macro expansions included.
Where the two values differ, the expression is written again with `+` for space, then if that doesn't
produce the same binary output, as the number macro1 gives.
If neither produces the correct result, the conversion fails.

**Mactoam1 never writes silently incorrect code.**

Anything it cannot convert becomes an *#error* line in the output.
Subsequent am1 assembly will give an error with a message
naming the source line, and mactoam1 exits with status 1.

## Usage

```
mactoam1 [-o outfile] [-t tabwidth] file.mac
```

- `-o outfile` writes the conversion to the given file else to stdout.
- `-t tabwidth` sets the tab stops used to expand tabs, 8 by default.

Messages go to stderr as `file.mac:line: error: ...` or `file.mac:line: warning: ...`, sorted by line.

The exit status is 0 when the file converted propery, 1 for a translation error, and 2 for a usage error.

The input can have LF, CRLF or CR-only line ends; the output has LF only (Unix style).

## What it converts

The output keeps the source's layout.
Each statement and comment will be where it orignally was with tabs expanded
and with the original alignment.

The first line, macro1's title, is kept as written.

| macro1 | am1 |
|---|---|
| `/ comment` | `// comment` |
| a tab between statements | `;` |
| `x/` origin | `x/` |
| `tag,`, several on one line | each tag on a line of its own |
| `100,`, which checks the location | a comment |
| a space that adds | `+`, unless OR gives the same value, as for an instruction and `i` |
| `+ - * & !`, left to right | parenthesized to keep macro1's order; `!` is `\|` |
| `[e]` | `(e)` |
| `(e` and `(e)` constants | `[e]`; see "Constants" below |
| `name=expr` | `#define name (expr)`, with `#undef` first on a redefinition |
| `name=.` | the tag `name,` |
| a value that depends on where it is located, `x=.-n` | the number macro1 gives |
| overbar variables, `variables` | `x,` and `table 1` for each, where *variables* was |
| `dimension a(n)` (DEC's MACRO, not macro1) | n words for `a` in the variables block |
| `define name a,b` ... `terminate` | `#define name(a,b)` ... `#enddef`, and `name()` for a define with no arguments |
| a tag inside a define body, `R` | *local* ... *endlocal* in the body, with `R` a local tag at its first word |
| `D=D+D` on an argument inside a body | the later uses of `D` written `(D)*2`, `(D)*4`, ... |
| `repeat n, stmts` | the statements written n times |
| `text`, `flexo` | the same where am1 encodes the characters the same way, otherwise the words as numbers |
| `char lx` | `char xl`, or the number for a shifted character |
| `decimal`, `octal` | the same, as statements of their own |
| `start x` | `start x`, at the end of the output; `stop` when there is none |
| `noinput` | a comment, assemble with `am1 -r` for a tape with no loader |
| a symbol that is an am1 word (`C`, `table`, `lia`) | renamed with a `_` after it, `C_`, with a warning |
| a digit-first symbol, `1j` | `_1j` |
| `xx`, `skip`, `clc`, which am1 lacks | a *#define* after the title, when used |

Symbols keep their case.
Macro1 identifies a symbol by its first 6 characters, so two longer names that agree in
the first 5 characters  are one symbol to it.
They are written as the first spelling seen, with a warning.

Arguments missing from a define's use are written `0` as macro1 does, extra ones are dropped with a
warning.

**Constants.**

am1 lays out a constants pool in its internal order, this changes nothing a program can see.
am1 pools a constant holding a symbol not yet defined by the symbol rather than by its value.
Where that would give two equal constants separate words when macro1 has one,
mactoam1 writes the constant as its number, `[n]`.
After *constants*, an origin puts the location where macro1 would when the pool is shorter in am1.

**start.**

In macro1 a `start` with no address does not end the assembly, the next line is the title of a
further program.
Each `start` becomes a comment and the last one with an address is written as am1's `start` at the end of the
output with a warning for any code after it which macro1 punches after the start.
The same data will be loaded.

## What it refuses

Each of these is a *#error* line in the output and an error on stderr:
- an assignment inside a define body, other than `D=D+D` on an argument, or to `R`
- an assignment of a constant's address, `x=(5`
- *constants* or *variables* inside a define body
- a `.` in an argument to a define that has its own `R` if it cannot be written as a number
- a tag inside a `repeat`, a `repeat` whose count differs from one use to the next, and a count with a space in
  it (macro1 adds the space, so DEC MACRO's `repeat 3 cli ...` repeats 04003 times)
- `decimal` or `octal` in the middle of an expression
- an expression whose value am1 cannot be given the way macro1 gives it

am1 will not accept an *#error* inside a *#define* block.
A refusal in a define body leaves a comment instead and an *#error* will follow the *#enddef*.

Two constructs refuse the whole file which will then be just the *#error*:
`expunge`, which empties macro1's symbol table, and a define used inside itself without end, more than 200
levels deep, which macro1 fails on.
DEC MACRO's `terminate` on a body line does this.

A line macro1 itself rejects is written as-is and a warning is
given using macro1's message, am1 usually rejects it too.

## Building mactoam1

In `Tools/MtoA`, type make or make install.
`Tools/Makefile` also builds it with the other tools and copies it to `bin/`.

The test suite is in `Tools/MtoA/Tests`, its `README.md` explains how to run it.
It converts every `.mac` file in the distribution and also a separate test for each construct above and checks that
am1 outputs exactly the words macro1 outputs.
