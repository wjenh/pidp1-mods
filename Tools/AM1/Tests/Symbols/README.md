# Tests/Symbols -- the line a label is recorded on

`symbols_check.sh` holds one thing:

> A global label's line in the `.sym` is the line it is defined on, in the
> file it is written in, whatever follows the label.

and the same for a `var` name: its line is the line it is declared on.

Run it with the testing build, `am1test`, which is its default (build it with
`make am1test` in the am1 directory):

```
./symbols_check.sh
./symbols_check.sh ../../am1test -p /path/to/an/older/am1
```

A clean run of the first form reports **9** pass and **0** fail. With `-p` it
is **12/0**: the three extra passes are checks 6 (a control leg per source)
and 7, which need an older am1 that records these lines wrongly, and so
cannot be part of the normal run.

## Why the line needs care

`lineno` counts the line *terminator*, so what it holds when a label's rule
reduces depends on whether bison has taken that terminator as its lookahead --
and that depends on what follows the label: a plain expression, nothing, a
`text`, `ascii` or `type340` trailer. So the line is recorded where the
label's comma is *scanned* (`lexer.l`, `parser.y`), which is on the label's
own line in every shape.

A `var` name is a global symbol and reaches the `.sym` too. Its line is
recorded where the lexer returns the name as `NAME` or `ADDR`, and the two
`varname` rules take it: the one for a name seen for the first time, and the
one for a name already referenced, as `fwdvar` is. `setVarsPC()`, which
places the var at a `variables` line or at the end of the program, does not
write the field.

## The oracles

Two, and each catches a mistake in the other.

1. **`*.expected`** states, name by name, the line each label is defined on,
   read off the sources' own numbering, not from any am1 output.
2. **Check 2 has `awk` derive the same thing** from the sources directly. So a
   mistyped expectation fails check 2, and a wrong `awk` fails against the
   hand-written file.

The expectations are absolute line numbers, which is the whole point, so
**do not insert or delete a line above a label** in these sources without
re-reading the `.expected` file beside it. Check 2 will say so if you do.

## The sources

| Source | What it is for |
|---|---|
| `sym_shapes_test.am1`, `.expected` | fifteen labels, one per shape: first word of the program, after a `//` comment, after a blank line, after a C-style comment block, nothing after the comma, a plain expression, a trailing `//` comment, two statements split by `;`, referenced before defined, `text`, `ascii` and `type340` trailers, a `jmp` target, an exported label, and the last label in the file |
| `sym_inc_test.am1`, `sym_inc_part.ah`, `.expected` | across files: a label before an include, three inside it, one after it returns. The include is padded so its numbering cannot coincide with the includer's |
| `sym_lib_test.am1`, `sym_use_test.am1` | the importer: the library exports a label on line 14, a line the ten-line importer does not have, and the imported symbol carries that line through the library's `.sym` |
| `sym_var_test.am1`, `sym_var_part.ah`, `.expected` | ten vars: alone, with a trailing comment, two on one line, initialized, after a C-style comment block, referenced before declared, in an included file, after the include returns, and one declared after `variables`, so placed at the end of the program; and two labels |

## The checks

1. the shapes: every (name, line) pair is the one `.expected` states, with no
   extra and no missing name -- the label shapes, then the vars;
2. the expectations are the sources' own numbering, derived independently
   (a var is each name in a `var` statement's list, less any `=value`);
3. across files: each label takes its line in the file it is written in;
4. the importer: an imported symbol keeps the line its own source gave it;
5. the `.sym` is still well formed -- six-digit octal address, one of `G`,
   `I`, `X`, and the one exported label marked `X`;
6. (`-p`) the control legs: the older am1 FAILS check 1 on each source, and
   fails on every shape named for it, so check 1 can fail;
7. (`-p`) nothing else differs: for four sources both binaries assemble,
   every output is byte-identical, and the `.sym` differs only in its last
   field (two builds that both fail would compare equal, so both must
   assemble);
8. a coverage guard over all of it.

## What it does not cover

- **The emulator.** The line field never reaches an assembled word, so there
  is nothing for a program to run differently. `Tools/AD1/eval.c`, the only
  other reader of a `.sym`, parses the address, the flag and the name and
  never looks at the line field.
