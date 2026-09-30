# The **am1** optimizer, a quick reference

This document tells you which flags and directives to use to make an **am1** program
smaller or faster, carefully or aggressively, and what can go wrong.
Everything here is covered in full in *UsingTheAm1Optimizer.md*; each section below
names the part of it to read next.

This is version 1.4 and covers am1 version 3.0; it will be updated as needed.\
Edit date 29-Sep-2026\

## Read this first

**The optimizer can break a working program, and the assembler will not tell you.**

A PDP-1 program can do things no assembler can fully see:

- patch its own instructions while it runs (*dap*, *dip*, *dac* into code)
- compute an address at run time, such as *law table* then *add n*, or a jump found by counting from a label
- execute data or a word it builds with *xct*
- depend on how long its code takes: delay loops for the typewriter, the punch, the display
- share memory with a device, a sequence break handler, the drum, DCS2 or a high speed channel
- fill memory at run time that no assembled word shows, such as a stack or a buffer

The optimizer finds and refuses the cases it can see.
It cannot see all of them, and some rewrites move every address after them, so a mistake
can show up somewhere unrelated to the code that was changed.

**Always assemble your program twice, with and without the optimizer, and run both.**
If you cannot test the program, use **-O** alone: it only gives advice.

## The three levels

- **-O** writes a report, *sourcefile.opt*, of places the source could be shorter or faster.
  **Nothing in the assembled program changes.** It is always safe.
- **-O1** rewrites, but only where you have declared it may, with the directives below.
  With no directive in the source it assembles exactly what **-O** does.
- **-O2** also rewrites where you have declared nothing, on a guess, and warns on every run
  that the guess can be wrong.

## Choosing the flags

| Goal | Careful | Aggressive, at your own risk |
|---|---|---|
| Just advice | *am1 -O prog.am1* | |
| Faster | *am1 -O1 prog.am1*, with *%%optimize* around code you have checked, and *%%speed* or *%%inline* where you want calls expanded | *am1 -O2 -O=undeclared prog.am1*, no directives needed |
| Smaller | *am1 -O1 prog.am1*, with *%%optimize* around code you have checked | *am1 -O2 -O=undeclared prog.am1*, no directives needed |

Add *-S* to any of them to see the highest address used in each bank, which is the
easiest way to see what a build saved.

### Careful

You mark the code you vouch for, and nothing else is touched.
Inside an *%%optimize* region **-O1** makes every rewrite it knows how to make:

- one word replaced by a faster one: *lac [5]* becomes *law 5*, *lac [0]* becomes *cla*,
  and a jump to a jump goes straight to the far target
- words deleted: a *cla* before a *lac*, a skip over a *jmp*, a *jmp .+1*, an *eem* or *lem* that changes nothing
- constant pool words no instruction reads any more, removed
- code that only one *jmp* reaches, moved to follow it so the *jmp* can go
- short counted loops, written out in full

Speed and space come from the same region: most of these save both time and words.
The one rewrite that trades words for time, expanding a call into a copy of the
routine, needs its own declaration, *%%speed* or *%%inline*.

A region is a promise. By drawing one you say that inside it:

- no word is modified while the program runs
- no word's address is used as a value
- none of it is there to take the time it takes
- no address is computed at run time across it

The report checks the first two against what it can see and names every word that
contradicts your region. It cannot check the last two. Keep delay loops, device code
and address tables out of your regions, or fence them off with *%%nooptimize*.

### Aggressive

**These recipes are dangerous. They are here because the assembler allows them, and you
take the risk.**

**-O2** with no region rewrites anywhere, one word for one word: the *law* and *cla* loads
and the shortened jumps above. Five heuristics guess where code is timed or shared with
a device or a handler, and leave those places alone. The guess cannot see an address the
program computes at run time. On its own, **-O2** makes a program slightly faster and
**no smaller**: without a region it deletes no word and removes no pool word.

*-O=undeclared* (with **-O2** only) lets the same guess do what a region does: delete
words, remove pool words no instruction reads any more, and move code anywhere. That
changes addresses. A sixth heuristic keeps out of a table whose address is loaded and
changed in one block, but it does not see arithmetic on an address outside the block that
loads it, nor an entry past the first label of the table: a jump table built that way
will break silently. Every heuristic still applies, so the delay loops, device loops and
handler code it can see are left alone. *-O=place=undeclared* is the same switch under
its first name.

*%%optimize* around the whole program, built with **-O2**, is the other aggressive way to
make a program smaller, and the riskier: inside a region the guess asks only about
handlers and computed tables. It tells the assembler that nothing anywhere in the program is
patched at run time, timed, or reached through a computed address. That is almost never
true of a whole program. **-O2** is used rather than **-O1** because inside a region
**-O2** still keeps out of code a sequence break handler can reach, and **-O1** does not
ask. Every deletion moves every word after it. **If any part of the program
computes an address, or depends on its timing, this build can be wrong with no warning
from the assembler**, and the failure may appear far from its cause. Fence off every delay loop, device loop, handler
and table you know of with *%%nooptimize* before you try it.

The most aggressive build of all adds *%%speed* around the whole program too, and raises
the limits below. It spends words to save time, and it carries every warning above.

## The directives

Each goes on a line of its own, and none of them generates any code.

| Directive | Means |
|---|---|
| *%%optimize* ... *%%endoptimize* | **-O1** and **-O2** may rewrite this code, including changing its length |
| *%%nooptimize* ... *%%endnooptimize* | no level ever rewrites this code, whatever else says it may |
| *%%speed* ... *%%endspeed* | calls in here may be replaced by a copy of the routine they call |
| *%%inline name* | any call to the routine *name*, anywhere, may be replaced by a copy of it |
| *%%ceiling expr* | in this bank, no rewrite may grow the program to *expr* or beyond; use it when the program fills memory there at run time |

```
%%optimize
go,     lac [5]
        dac a
%%speed
        jsp addxy
%%endspeed
%%nooptimize
        law i 144               // a delay: keep it as written
        dac cnt
dly,    isp cnt
        jmp dly
%%endnooptimize
        jmp go
%%endoptimize
```

Regions, spans and speed regions do not nest within their own kind, must end in the file
they began in, and an unclosed one stops the assembly.

## The limits

These only matter with *%%speed*, *%%inline* or loops inside a region. All are decimal.

| Flag | Default | Sets |
|---|---|---|
| *-O=inline=cap:N* | 8 | the largest routine body, in words, copied in place of a call |
| *-O=inline=reserve:N* | 64 | the words left free in each bank when copying |
| *-O=unroll=cap:N* | 64 | the largest a loop may become when written out |

A routine called only once, and one named by *%%inline*, is copied whatever its size,
as long as the bank has room.

## Before you trust a build

1. Assemble the program without the optimizer and with it, and run both the same way.
2. Read the report, *sourcefile.opt*. Its Transforms section, and the sections at its
   end, list every word that was rewritten, moved, copied or deleted, and why each other
   candidate was not.
3. *-O=xform*, given with the level you build with, prints the same record on stdout,
   one line per candidate.
4. *-O=source* writes the program back out as am1 source, to *sourcefile.opt.am1* beside
   the source. A line the optimizer did not change is copied as you wrote it, names and
   *#include* lines kept; a changed line is written out after a *was:* line quoting it,
   and an include file with a change is written out in its *#include*'s place. Assemble
   it as its header says: as the source was, with the *-D* and *-I* it names, or with
   **-n** when am1 has said it wrote every line from the tree. Under **-O** it gives
   the same tape.
   Under **-O1** and **-O2** it gives the optimized build's tape, and a comment beginning
   *am1 -O1:* or *am1 -O2:* marks every change. Assemble it without **-O**, or it is
   optimized again. If its header says *NOT the optimized tape*, its constant pool is in
   another order; see "Writing the program out as source".

## When a build breaks

- Put *%%nooptimize* ... *%%endnooptimize* around the code you suspect, or narrow the region.
- *-O=upto=N* keeps only the first *N* rewrites, in *-O=xform*'s order. Find the largest
  *N* that still works: rewrite *N*+1 is the one that broke it. *-O=upto=0* keeps none.
- The script *am1bisect.sh*, in the **am1** source directory, does the halving for you,
  given a command that tests the build.
- *-O=off=file* switches off the rewrites the file lists, one *bank address* per line,
  copied from an *-O=xform* line.

These work only with **-O1** or **-O2**.

## Where to read more

In *UsingTheAm1Optimizer.md*:

- what a region promises, and how the report checks it: "Declaring where a rewrite is allowed"
- the guess and its five heuristics: "-O2: rewriting on a guess"
- inlining, code placement, unrolling and deletions: "Declaring where a length-changing rewrite may happen"
- *%%ceiling*: "A bank's ceiling"
- bisection and *am1bisect.sh*: "Finding the rewrite that broke a build"
- every section of the report: "The report, section by section"
- *-O=source*, the program written back out as am1 source: "Writing the program out as source: -O=source"
