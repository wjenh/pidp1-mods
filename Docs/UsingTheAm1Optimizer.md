# Using the **am1** optimizer advisor, `-O`, `-O1` and `-O2`

This document describes the `-O` flag and the report it writes, and the `-O1`
and `-O2` flags, which act on the code.
For **am1** itself, see *UsingAM1.md*. For which flags to use and what can go
wrong, in a few pages, see *UsingTheAm1OptimizerQuickReference.md*.

This is version 1.34 and covers up through am1 version 3.0; it will be updated as needed.\
Edit date 30-Sep-2026\

## What the advisor is

`-O` makes **am1** read the program it has just assembled and write a report
naming places where the source could be shorter or faster.
It is an advisor, no code is changed.

`-O1` modifies code, and it is narrow in scope on purpose.
It rewrites a word only where you have declared a region,
only where a finding has passed every check the advisor can make,
and only for the five rules that replace one word with another at the same address.
Nothing moves.
See "`-O1`: rewriting inside a region" below.
Without a region anywhere in the source, `-O1` does nothing.

`-O2` makes the same five rewrites without asking for a region.
It guesses instead, and the guess can be wrong, every `-O2` assembly says so on stderr.
See "`-O2`: rewriting on a guess" below.

`-O2 -O=undeclared` goes one step further.
It lets the same guess stand in for a declaration for the rewrites that make the program shorter: space mode's deletions, the pool reclaim, the extend window's deletions and fall-through placement.
Every word after one of those moves.
See "`-O=undeclared`: shortening the program on the guess" below.

Neither rewrites anything inside a `%%nooptimize` span.

Both levels make one more rewrite, and it is the one that moves things: inline expansion.
It happens only where you have declared it with a `%%speed` region or an `%%inline` marking.
See "Inline expansion" below.

This is worth stating again because it is unusual.
An optimizer normally optimizes.
This one does not, for two reasons that are not going away:

- A PDP-1 program can be its own data. It can patch its own address fields,
  compute jump targets, execute words in place with `xct`, and share memory
  with a device or a sequence break handler. Some of that the assembler can
  see; some of it it provably cannot.
- Most of the rewrites worth making change the number of words, which moves
  every address after them. Doing that safely means relaying out the program,
  and relayout is exactly what breaks a program that names its own addresses.

So the advisor finds the patterns, checks what it *can* check, states plainly
which check it cannot make, and leaves the decision with the author.
`-O1` takes only the part of that decision that neither reason touches: a rewrite
that moves nothing, in code the author has said is not modified, not
address-taken and not timed.

## Running it

**am1** [ *the usual flags* ] **-O**[**1**|**2**] [**-O=**modifier]... sourcefile

`-O` writes `sourcefile.opt` beside the other outputs. It combines with
everything else, so the ordinary way to use it is to add it to a build that
was already producing a listing:

```bash
am1 -b -d -S -O adventure.am1
```

The installed **am1** takes these modifiers:

| Modifier | What it does |
|---|---|
| `-O=xform` | prints on stdout what `-O1` or `-O2` does with every live finding: rewritten, or which reason it was not; without either, a dry run of `-O1`'s one-word-for-one-word rewrites alone, which rewrites nothing and lists every deletion as not carried. With a speed declaration, also one `S1` line per declared call site: copied, or which reason it was not (see "Inline expansion"). With any region, also one `S4` line per declared placement site: moved, or which reason it was not (see "Fall-through placement"), and one `S2` line per declared loop: unrolled, or which reason it was not (see "Loop unrolling") |
| `-O=upto=N`, `-O=range=B:LO-HI`, `-O=off=FILE` | with `-O1` or `-O2`, switch rewrites off, to find the one that broke a build. See "Finding the rewrite that broke a build" |
| `-O=inline=cap:N`, `-O=inline=reserve:N` | set inline expansion's body cap (8 words by default) and each bank's reserve (64 words by default), decimal, 0 to 4096. See "Inline expansion" |
| `-O=undeclared` | under `-O2`, let the guess license space mode's deletions, the pool reclaim, the extend window's deletions and fall-through placement where no region declares them. Refused without `-O2`. `-O=place=undeclared` is its first spelling, and does the same. See "`-O=undeclared`: shortening the program on the guess" |
| `-O=unroll=cap:N` | set loop unrolling's cap, the most words a loop may become (64 by default), decimal, 0 to 4096. See "Loop unrolling" |
| `-O=source` | write the program, as the level left it, as am1 source to `sourcefile.opt.am1` in the source's directory. Alone it runs the advisor, as `-O` does. See "Writing the program out as source: `-O=source`" |

Any other modifier is refused with the usage text.

### Writing the program out as source: `-O=source`

`-O=source` writes the program to `sourcefile.opt.am1`. The file goes in the
directory the source is in, not the working directory where the other outputs
go. It is written after the optimizer, and under `-O1` or `-O2` after
relayout, so it holds the program as that level left it. Each `%%` directive
is written where it stood, and no new one is added.

```bash
am1 -b -O1 -O=source adventure.am1        # writes adventure.opt.am1
am1 -b adventure.opt.am1                  # the optimized build's tape
```

**A line the optimizer did not change is copied** from the source, or from
the include file it came from, exactly as you wrote it: its names, macro
calls and comments, and every `#define`, `#if` and blank line around it. An
include file with no change stays its `#include` line. So the file goes
through cpp again, and is assembled as the source was. Its header comment
names:

- the source the file was written from;
- the date and the level;
- the `-D` and `-I` flags to assemble it with, which are the ones the source
  was built with. A source built with `-n` gives a file to build with `-n`.

The first line is the source's title line, which is punched on the tape.
Under plain `-O` nothing is rewritten, so the file is the source itself with
the header after its title, and it assembles to the same tape.

**A line the optimizer changed is written from the program as assembled.**
A comment line quotes the line it replaces:

```
// am1 -O1: was: w1,     lac [LIMIT]             // the limit
w1,	law	5; local am1gone; endloc  // the limit  // am1 -O1: T8d, was lac [5]  // am1 -O1: after
    the word, what the old one named first, so the pool keys hold
```

(The written line is one line; it is folded here.)

A written line has numbers where the source had names, and a macro call
comes out as its expansion. When a change falls inside an include file, that
file is written out in place of its `#include` line, between two comment
lines naming it, and so is each include around it. A quoted `#include`
inside it that named a file beside it, in another directory, is rewritten to
the path cpp found that file by, with a note.

Under `-O1` and `-O2` the file gives the optimized build's own tape. Each
change carries a comment beginning `am1 -O1:` or `am1 -O2:`, the level as
run:

- a rewritten word ends its line with the rule and the old word, e.g.
  `// am1 -O2: T8d, was lac [dest]`;
- a deleted word's line keeps its label and comment, and ends with
  `// am1 -O1: deleted <word>`;
- a moved run is written where it now runs, between `moved here` and
  `the moved run ends` lines, and a line marks where it stood;
- an inline copy, an unrolled body and an `-O=edits` copy are bracketed by
  `begins` and `ends` lines.

Assemble the file **without `-O`**, or it is optimized again: its `%%`
directives are still in it.

A few spellings in a written line exist only to keep the tape:

- `table 0, [expr]` emits no word. It names a constant, so the pool holds its
  word where the optimized pool has it.
- After a T3 or T8 word, `; table 0, name` or `; local am1gone; endloc` makes
  again what the old word made first, so the pool keeps its order.
- A T3 whose target has no name is written from the nearest label below it,
  e.g. `jmp go + 4`. A T8d constant that held an address is written as its
  own expression, e.g. `law dest`.
- A copied routine's local is written `. + N`, because the copy stands
  outside the routine's scope.
- A name written just before a parenthesized operand is written in
  parentheses, `(name)`. The source did not follow it with `(`, but a
  function-like macro may share its name, and cpp would take the pair for a
  call.
- Where the source switches to `decimal`, a number in a written line carries
  `0o`, which reads as octal in either radix.

**When the lines cannot be followed.** am1 pairs each statement with its
source line through cpp's line markers. If that pairing fails, or the
optimized order is one the copied text cannot give, am1 says why on stderr
and writes every line from the program as assembled, after cpp, as
`-O=source` did before it copied lines. That file is assembled with **-n**,
and its header says so. A source whose title line comes from an include is
one such case.

**One build cannot be written exactly.** Under `-O2`, a T8 that no region
declares keeps the pool word it stops naming. If relayout then moves that
word's old address, no source line can give it the same place in the pool.
The file's header and stderr say "NOT the optimized tape". Declaring a
region over the rewrite removes the case, because the reclaim then takes the
word.

All of this is checked on real programs: every `.am1` in the repository
round-trips under plain `-O`, and every optimizer test source
and Adventure's regions give the optimized tape under `-O1` and `-O2`.

### The testing build: `am1test`

The optimizer has fifteen more modifiers, each of which exists to test it:
the debug dumps of each analysis, the decoder's self-check, and relayout's
edit file. The installed **am1** does not
recognize them, so they do not clutter its usage text. **am1test** does. It
is **am1** built with `AM1_TEST_SWITCHES` defined, by `make am1test` in
`Tools/AM1`, and it is never installed. `wordtable_check.sh` and the
`Tests/Symbols` suite run it (see "Testing"). Apart from these switches, it
assembles every program byte for byte as **am1** does. The one other difference is
under `-O=xform` with `-O1` or `-O2`: **am1test** then also analyzes the
rewritten program a second time and prints its `second` lines (see "What it
reports").

Wherever this document names one of the modifiers below, it is **am1test**'s.
Each dump prints on **stdout**, on top of the report. They are for working on
the optimizer itself, but two of them are useful for reading a program, and
two more for reading its structure:

| Modifier | What it prints on stdout |
|---|---|
| `-O=dump` | the word table, one line per emitted word, in `-T` format |
| `-O=decode` | the same table with each word decoded and its source spelling classified |
| `-O=refs` | every reference edge, with its role, and the per-word flags |
| `-O=flow` | the basic blocks, the control-flow edges, the sequence-break enable commands, reachability, and where each call returns to |
| `-O=calls` | the routines, the call graph over them, its cycles, the sequence-break enable commands and the sequence-break pool |
| `-O=share` | the return words the report proposes sharing, with every routine's color, pool and group |
| `-O=regions` | the `%%optimize`/`%%endoptimize` regions, their extents, and every word whose region's declaration the evidence contradicts; then the `%%nooptimize` spans, when the source declares one; then the `%%speed` regions and the `%%inline` markings, when it declares one of those. `-O1` and `-O2` act on the speed declarations: see "Inline expansion". Then one `ceiling` line per `%%ceiling`, when the source declares one: its bank, value, whether it is the one kept, its line and its file |
| `-O=rules` | the findings, one per line, live and suppressed |
| `-O=scratch` | the scratch-pool measurement: every storage local, the class its rules put it in, and what sharing the pool could free, block-local and pooled, per bank. The report's scratch-word sharing section is written from it |
| `-O=guess` | what `-O2`'s heuristics mark: the loops and their cycles, each classed plain, delay or device; the handler territory; device buffers; addresses written as numbers; the runs of code a computed address indexes; and every live finding with the heuristics that refuse it. A measurement: it refuses nothing and changes no word |
| `-O=values` | what each basic block already knows about AC and IO: every load whose register already holds the value, or that one word could replace (`lai`, `lia`, `law`), and every load or clear whose result the block overwrites unread; with the safety flags on each, the per-bank counts and what the pass cannot see. A measurement only: it changes nothing, and no finding comes of it |
| `-O=speed` | what speed mode could do, site by site: inline expansion, loop unrolling, jump chains followed to their end and fall-through placement, each site eligible or refused with one reason, what an eligible site saves per execution and spends in words, with tallies per bank and each bank's free words, counted to a declared `%%ceiling` where there is one and marked `(declared)`. A site whose callee could not be copied at all -- a label defined in the copy, or a copied word naming a word of the body -- is refused `notcopyable`, and the line names the word that broke it. A placement whose `jmp` the program uses as a word is refused `jmpused`, and one whose code reaches a `hlt` is refused `falls`. A loop whose body has a label but its own or names a word of the loop is refused `internal`; one with a word something may write, or whose setup, `isp` or `jmp` the program uses as data, `used`; one whose body reads AC before loading it, `acentry`; and one whose next code reads AC before loading it, `acexit`. A measurement only: it changes nothing, and adds nothing to the report |
| `-O=relayout` | lays the program out again, after any `-O=edits` edits, and prints what happened: each edit accepted or refused with its reason, where every moved word went, where every copy went, and the pool slots gained and lost. A testing instrument: with no edit it changes nothing. See "Testing relayout" |
| `-O=window` | the extend-window analysis: one line per reached `eem` and `lem` with its class (`redundant`, `freed`, `dead`, `needed`, `refused` with its reason, or `unreached`) and source line, one per hazard (`proved`, `unproved` or `possible`) with its pointer, one per reverse hazard (`window reverse`, the same classes, `return` when the pointer is a return word), one per indirect reference an unreached `eem` or `lem` guards (`window unexamined`, with its pointer when known), then, under `-O1` or `-O2`, one `window delete` line per declared deletion with its kind and fate, then the totals. A program with no `eem` or `lem` at all prints `window: no reached eem or lem`; one whose every `eem` and `lem` is unreached gets the dump. The dump itself changes nothing. See "The extend window: `eem` and `lem`" |
| `-O=check` | runs the decoder's built-in self-check first, then proceeds |
| `-O=edits=FILE` | not a dump: make the edits FILE lists, then lay the program out again. Relayout's test instrument; see "Testing relayout" |
| `-O=reclaim=off` | not a dump: hold the pool reclaim off, so that a check whose oracle is one word for one word can still judge the same-length rewrites. It switches off no rewrite and changes no fate |

`-O=decode` is the one to reach for when you want to know how **am1** read a
word; `-O=flow` when you want to know why something is being called unreached.

`-O=calls` prints the layer above the blocks. **am1** programs have no
procedure construct, so a *routine* is derived rather than declared: its ENTRY
is a block whose first word something calls (`jsp`, `jsp i`, `jda`, `cal`),
its BODY is the blocks reachable from that entry without passing through
another entry, and its RETURN words are the ones that leave through
`jmp i rtn` or, for the `jda` form, `jmp i subr`. The dump gives one line per
routine -- entry, call idiom, block and word counts, call sites by idiom,
return word and flags -- then the call arcs, the transitive closure over them,
the cycles with each one classified, and the routines a sequence-break handler
can reach.

Three shapes are counted rather than decided quietly, because each of them is
a real program's doing and not an error: a block that is both an entry and
inside another routine's body (something falls into a routine's first word); a
body two entries share, which is one routine with two entries and not two
routines; and a body reachable only through an indirect jump nothing could
follow. The counts are on the `categories:` line and each instance is listed
under it.

`-O=share` is the working behind the report's return-word sharing section. It
prints one line per routine -- its return word, that word's bank, which pool it
is in, the color the assignment gave it and which group it went into -- then
one line per group with its members, then a per-bank reconciliation against the
call-depth figures. A reader who does not believe a group can check it there
without reading any C.

`-O=regions` is the working behind the report's region section. It prints one
line per region — the bank and address it starts and ends at, its word count,
the lines the two directives are on and the file they are in — and then one
line per word inside a region that the reference analysis saw written, patched
or with its address taken. On a source that declares no region, which is most
of them, it prints one line saying so.

### When the optimizer fails, and `-p`

If the optimizer fails, the `.opt` file is removed rather than left half
written. Under `-O` the rest of the assembly is unaffected either way. Under
`-O1` or `-O2` a failure of one of the rewrite's own checks stops the assembly
instead, before anything is written; see "`-O1`: rewriting inside a region".

`-p` prints the parse tree straight after the parse, before the optimizer
runs, so under `-O1` or `-O2` it shows the program as written, not as rewritten. The
four outputs -- tape, listing, macro1 source and test dump -- all show the
rewrite.

## Declaring where a rewrite is allowed: `%%optimize` and `%%endoptimize`

Under `-O1` a region is where **am1** is allowed to rewrite, and the only place.
Under `-O` alone the two directives record a declaration and check it, which is
useful on its own: they say which code you vouch for, and the report argues
with you about it.

```
%%optimize
        cla
        lac count
%%endoptimize
```

The assembler's manual, `UsingAM1.md`, has the syntax, the four errors and the
guarantee that the directives cost nothing — no word emitted, no location
moved, and not a byte of difference in the tape, the listing, the macro1 source
or the test dump. What matters here is what a region *means*.

### The report advises everywhere. A transform will fire only inside a region.

That sentence is the whole design and it is worth reading twice, because the
obvious reading of "the directive replaces precondition P5" is the wrong one.

A region is your permission to **rewrite**. It is not a condition on being
**told**.

- **A source with no region anywhere** — which is every program written so far,
  `adventure.am1` included — gets exactly the report it always got. P5 is not
  checked, the header says so in the same words, every finding is live, and
  every count is unchanged. The report gains one short section saying no region
  was declared, and nothing else. If it did anything more than that, all 239 of
  Adventure's live findings would vanish and the report would go blank in the
  one place readers use it.
- **A source with at least one region** gets P5 checked. A finding inside a
  region is live and marked *inside a declared region*; a finding outside every
  region is **still live, still counted and still printed in full**, and marked
  *outside every declared region, so a transform would not fire here*. Nothing
  is withheld and nothing moves to the suppressed list.

The suppressed list is for patterns the evidence refuses. A pattern you have
simply not opted in yet is not refused.

A pattern that straddles a boundary — some words inside, some outside — is
marked *straddling*, and a transform would not fire on it either. A rewrite
takes the whole pattern or none of it, so half a permission is none. Moving the
directive by one line is usually all such a case needs.

The mark appears on **live findings only**. A refused pattern never carries
one, wherever it sits. Where a pattern the evidence has already refused happens
to lie is not something you can act on, and adding that a transform would not
fire on it either would be noise on the one list you read for reasons. Nothing
is hidden by leaving it off: the usual reason a pattern inside a region is
refused is that one of its words is written or has its address taken, and the
Optimize regions section names every such word against the region it
contradicts.

### Membership is per emitted word

A region holds the words emitted between the two directives, in the order they
are emitted. It is not a range of addresses, and that matters in three ordinary
cases:

- a `bank` directive inside a region. The words carry on being in it, in the
  new bank, and the report says the region spans a bank change. The first and
  last addresses may then be in the wrong numeric order, which is exactly why
  addresses are not what membership is decided by.
- an overlay, where two words are emitted at the same address.
- a **cpp** macro that expands to several statements on one line. All of the
  words it expands to are in the region; there is no question about which half
  of a line was inside.

Variables and pooled constants are **never** in a region, even when the `var`
or the constant reference is written inside one. Those words are placed after
the whole source has been read, by which time every region has closed.

### Your declaration, checked against the evidence

A region asserts four things about the code inside it:
- no word of it is modified while the program runs;
- no word's address is used as a value;
- none of it is there to take the time it takes;
- no address is computed at run time across it.

The advisor already knows the first two from the reference analysis, so it
checks them:

> A word inside a region that the analysis saw written, patched, or with its
> address taken is a **contradiction between your declaration and the
> evidence**, and the report names it — which region, which address, and what
> was seen.

That is the most useful thing this part of the advisor produces, because it is
precisely the case in which a transform could corrupt a working program on
your own say-so. Either the region is drawn wider than you meant, or the
reference is one you know to be harmless — but it is worth being sure which.
`-O1` does not refuse a region the evidence disputes: each word it rewrites
there has passed P1 to P4 on its own evidence. But its Transforms section
repeats the warning and names every rewritten word that sits in such a region.

The third assertion, timing, is not checked and cannot be. Neither is a word
the drum overwrites behind the program's back.

The fourth matters once a rewrite changes a program's length. Such a rewrite
moves every word after it, up to the next origin, and that includes words
outside the region. A `law base` / `add n` pair that computes an address past
an edited word would then compute the wrong one. Inline expansion, placement,
unrolling and space mode's deletions all change it (see those sections, and
"Testing relayout"). Relayout checks the static cases: an edit is refused if a
number, an offset or a `.` would name a different word. A computed address it
cannot see. Timing, the drum and computed addresses are
still yours.

## Keeping every level out: `%%nooptimize` and `%%endnooptimize`

A region says where a rewrite is allowed. A **span** says where none is, at any
level, whatever the analysis or `-O2`'s guess says:

```
%%nooptimize
dly,    law i 144
        dac cnt
dlyl,   lac [5]
        isp cnt
        jmp dlyl
%%endnooptimize
```

A finding with any word of its pattern in a span is left alone, and so is a T3
whose intermediate jump, the word it reads through, is in one. The finding is
still advice: the report prints it among the findings as always, and the
Transforms section puts it down as *inside a nooptimize span*. Put a span
around a delay loop, device timing, a handler, or anything else you know must
stay as written.

The rules are the region's, with one difference. Each directive is a line of
its own. Spans do not nest, a span must end in the file it began in, and one
still open at the end of the source is an error. Each of those stops the
assembly, with the line and file the span opened at. A span is checked against
nothing, since it can only withhold a rewrite, and so it cannot be wrong in the
dangerous direction.

Spans and regions are independent, and may overlap: a span inside a region
fences part of the region off. Neither directive changes a word of the
program. Without `-O1` or `-O2` a span changes nothing but the report, which
lists every span in a Hands-off spans section when the source declares one.

## Declaring where a length-changing rewrite may happen: `%%speed`, `%%endspeed` and `%%inline`

Everything above is about a rewrite that puts **one word in place of one
word**. A rewrite that makes code longer or shorter is a different thing, and
it needs a different permission, because a same-length rewrite that is wrong
breaks one word and you can find it in the `.lst`, while a length-changing one
that is wrong **moves every address after it** and the failure turns up
somewhere unrelated.

So there are two ways to say a length-changing rewrite may happen, and
`-O2`'s guess is not one of them. Fall-through placement, the smallest such
rewrite, has two more, and "Fall-through placement" below gives them: an
optimize region licenses it too, and so, if you ask for it by name, does the
guess. Space mode's deletions, the smallest of all, the pool reclaim and the
extend window's deletions have the same two. The name to ask by is
`-O=undeclared`: see "`-O=undeclared`: shortening the program on the guess".
Loop unrolling has one of those two: an optimize region licenses it too, and
the guess never does.

A **speed region** is the span form:

```
%%speed
        jsp getbits
        jsp getbits
%%endspeed
```

An **`%%inline` marking** names one routine, which may then be expanded wherever
it is called:

```
%%inline getbits
```

The marking is a directive and not something written on the label's line, so
it can sit in an include file beside the routine it names, and it may stand
before or after the routine: the name is matched once the whole source has
been read. It names the label on the routine's **entry word**, and any label on
that word will do when there is more than one.

A speed region is **not** an `%%optimize` region and does not imply one. They say
different things -- `%%optimize` says P5 holds here, and `%%speed` says spend words
here -- and a single region meaning both could not be withdrawn by halves. A
word may be in either, both or neither.

The speed region's rules are the other two spans': each directive is a line of
its own, speed regions do not nest, one must end in the file it began in, and
one still open at the end of the source stops the assembly. That last is the
least forgivable of the three unclosed spans, and for a reason worth saying
plainly: a missing `%%endoptimize` offers the rest of your file to a rewrite that
changes one word for one word, and a missing `%%endspeed` offers it to one that
moves every address after it.

Under `-O`, nothing acts on either declaration. They are recorded, and the
report lists them in a Speed declarations section, with the `-O=regions` dump
carrying the same figures. **Under `-O1` and `-O2` they license inline
expansion, fall-through placement, loop unrolling and space mode's
deletions**, below. A marking that names no routine is reported and not
refused, so a marking left behind by a rename costs nothing, and being told
beats being stopped. A second marking of a routine already marked is reported
as a duplicate. A source that declares no speed region and no marking prints no
section and no dump line at all, which is every program in this tree today.

### Inline expansion

Under `-O1` or `-O2`, a **declared** call site is replaced by a copy of the
routine it calls. Declared means the call word is in a speed region, or the
routine is marked `%%inline`. Every other call stays a call, whatever
`-O=speed` says it would save.

```
%%speed                                 %%speed
        jsp addxy            becomes            lac x
%%endspeed                                      add y
                                                dac z
addxy,  dap addxyrt                     %%endspeed
        lac x
        add y
        dac z
addxyrt, jmp 0
```

The copy is the routine's body less its first word, which saves the return
address, and less its last word, which must be the return, since the copy
simply falls through into the caller's next word. A return anywhere else in
the body, and a `jmp` to the last word, become `jmp .+k` in the copy, which
also lands on the caller's next word. A `jda` stays, as a `dac` to the same
word, with the copy after it, because the routine reads its argument from
there. When the site copied was the routine's **only** call, the routine is
deleted (for a `dac`-form routine, its return word too), and the program
gets shorter as well as faster.

Which sites are copied is decided for every site before any word moves, in
this order:

1. **The routine must be copyable.** Every reason `-O=speed` refuses a site
   applies (the line's `refused` fate). One more joined them in this version.
   A routine whose last word is not its return, or whose words are not one run
   from its entry, has no place a copy could end, and is refused `notcopyable`,
   naming the word.
2. **The spans and the guess.** A site or routine in a `%%nooptimize` span is
   left alone (`handsoff`). Under `-O2` the heuristics are asked as for any
   rewrite, H1 alone inside an optimize region and all five outside one, and
   any mark on the site or the routine refuses it (`guessed`). At every
   level, H6 refuses a copy whose site is in a run a computed address
   indexes. The guess can only refuse. The declaration is what licenses the
   copy.
3. **The edit must be writable** (`unrepresentable` if not). A `jda` must name
   its word by one plain name, since the `dac` is written with it, and a
   retargeted return must not hold a constant.
4. **A routine with one call, and a marked routine**, are copied whatever
   their size, as long as the bank has room below its ceiling (07751 in bank 0,
   the end of the bank elsewhere, or lower where the source declares one: see
   "A bank's ceiling" below). A copy that does not fit is `full`.
5. **Everything else** must have a body of at most the cap, 8 words
   (`-O=inline=cap:N`); a longer one is `cap`. The rest fill each bank's free
   words **cheapest first**, microseconds saved per word spent, and stop at
   the reserve, 64 words (`-O=inline=reserve:N`). A site the reserve stops is
   `budget`. A site the bank has no room for at all is `full`.
6. **No copy holds another.** A site inside the body of a routine another copy
   is being made of, or whose own routine's body holds a copied site, is
   `nested`. Bisection can then switch any copy off on its own.

Every copied site's line in `-O=xform` reads

```
xform fired S1 0 0203 prog.am1:12 callee 0 0224 mr body 6 copy 4 spent 3 save 25..20 single net -4 speed | jsp mr => copy 0225-0230 retarget 0227 delete 0224-0231 and 0232
```

It gives the site's bank, address and line, and the routine's entry. `body`
is the routine's size, `copy` the words the copy has, and `spent` what the
site grows by. For a routine with one call, `single net` is what the whole
program grows by once the routine is gone, usually a negative number. `save`
is the microseconds saved per execution, at best and at worst by which return
is taken. `speed` or `marked` says which declaration reached the site. After
the `|` come the call as written, the addresses copied, each word retargeted,
and what is deleted. A site that was not copied says why in the fate's place,
`cap` adding the cap. The summary line gains `s1sites`, `s1fired`, a count per
fate, `s1words` and `s1us`, printed only when the source declares something.

The copy itself is made by relayout (see "Testing relayout"), which lays the
program out again and checks every word that remains, as for any edit. A copy
relayout refuses is left a call, and the report and stderr both say so.

Nothing is ever copied outside a declaration. The saving printed is per
execution of the site. How often a site runs is not in the source, so which
declarations pay for their words is for you to measure.

### Fall-through placement

Under `-O1` or `-O2`, a `jmp` to code that only that `jmp` reaches is
deleted, and the code is moved to follow the place the `jmp` was. The code
moved is the **run**: the target's block and every block it falls into, up to
and including the first that ends in a jump no skip can pass. A `jmp` one word
after a skip ends nothing: when the skip is taken, control goes past it to the
next word, so that word is in the run too.

```
        lac x                           lac x
        jmp tail            becomes     add y
back,   ...                             dac z
                                        jmp done
tail,   add y                   back,   ...
        dac z
        jmp done
```

Nothing is copied and nothing is added: the program is one word shorter, and
each pass through the site is 5 microseconds faster. A label on the deleted
`jmp` moves to the run's first word, which is where a jump to it went. A label
standing on a line of its own before the run's first word goes with the run,
and so do any comment or blank lines between it and the word.

**What licenses it.** The `jmp` must be in an **optimize or a speed region**.
Either is your word that a length-changing rewrite may happen there, and this
one moves code a short way and adds no word. Every word of the run must be
declared too; a declared `jmp` with a run that is not is `partial`, and stays.

**`-O=undeclared`** licenses it everywhere else as well, on `-O2`'s guess,
including the run of a declared `jmp`.
It licenses space mode's deletions, the pool reclaim and the window's deletions too; "`-O=undeclared`: shortening the program on the guess" says what it needs and what it cannot see.
`-O=place=undeclared`, its first spelling, still works, and now licenses the same.
A line moved under the switch says `undeclared` where a declared one says `region`, and the report marks the move `(undeclared)`.

A site's fate is decided before any word moves, in this order:

1. **The judge.** The `jmp` must end a reached block, its target must start a
   block the `jmp` is the only way into, and the run must end in a jump.
   Otherwise the site is `refused`, with the reason `-O=speed` gives:
   `afterskip` (the `jmp` follows a skip, which would then skip the run's
   first word instead); `jmpused` (the program reads the `jmp`, executes
   it by `xct`, takes its address, or might reach or write it through a
   pointer); `entry` (the target's address is taken, so something else may
   jump there); `inrun` (the run holds the `jmp` itself); `falls` (the run
   reaches a `hlt`, the end of the code or something that is not code, so it
   does not end in a jump; after a `hlt`, Continue resumes at the next word).
   A `jmp` that is not a site at all, because its target has other ways in, is
   not listed.
2. **The declaration**, as above: `partial`.
3. **The spans.** A `jmp` or a run word in a `%%nooptimize` span is `handsoff`.
4. **The guess.** Under `-O2` the heuristics are asked, H1 alone when the
   `jmp` is inside an optimize region and all five otherwise, and a mark on
   the run refuses it (`guessed`). At every level, H6 refuses a `jmp` in a
   run a computed address indexes. As for every rewrite, the guess only
   refuses.
5. **No move touches another.** A site whose `jmp` or run holds a word
   another move, an inline copy or a T3 or T8 rewrite already touches is
   `overlap`, in address order; so of two `jmp`s each in the other's run, the
   first moves. Bisection can then switch any move off on its own.

Every site's line in `-O=xform` reads

```
xform fired S4 0 0242 prog.am1:92 target 0272 at run 3 save 5 region | jmp at => move 0272-0274 after 0242, delete 0242
```

It gives the `jmp`'s bank, address and line, the target and its label, the
run's length in words and the microseconds saved, and `region` or
`undeclared`. After the `|` come the `jmp` as written and the edit. A site not
moved says why in the fate's place, a `refused` one adding the judge's reason
and a `guessed` one the heuristics. The summary line gains `s4sites`,
`s4fired`, a count per fate and `s4us`, printed only when a site was recorded.
The report's Fall-through placement section lists every site with what
happened to it.

The move itself is relayout's `move` and then its `delete` (see "Testing
relayout"). A move or a delete relayout refuses leaves the code where it was,
and the report and stderr both say so.

### Loop unrolling

Under `-O1` or `-O2`, a loop counted by a constant is written out: its body
n times, one copy after another, and the setup, the `isp` and the `jmp` back
are deleted.

```
        law i 3                 becomes    h1,     lac na
        dac c1                                     add one
h1,     lac na                                     dac na
        add one                                    lac na
        dac na                                     add one
        isp c1                                     dac na
        jmp h1                                     lac na
                                                   add one
                                                   dac na
```

The count is `law i n`, or `lac K` where `K` is a constant word holding -n,
just above the header. `isp` on -1 leaves +0 and skips, so the loop runs n
times; that was measured on the emulator before this was built. The counter
word itself stays where it is, since it may be a variable relayout cannot
delete, but nothing names it any more. The loop's label stays on the first
copy, and a label on the setup moves there too, which is where a jump to the
setup went.

Each pass saves the setup, n `isp`s and n - 1 jumps back. It spends
(n - 1) x B - 4 words for a body of B words, which is negative for a short
loop run once or twice.

**What licenses it.** The `isp` must be in an **optimize or a speed region**,
and so must every word of the loop, setup to `jmp`; a declared `isp` with a
word that is not is `partial`, and stays. There is no switch to unroll
undeclared loops: `-O=undeclared` does not reach them.

A loop's fate is decided before any word moves, in this order:

1. **The judge.** Every reason `-O=speed` refuses a loop applies (the line's
   `refused` fate, with the reason): `runtime`, `entry`, `counter`, `call`,
   `exit`, and four more that the rewrite itself needs:
   - `internal`: a body word other than the first carries a label, which each
     copy would define again, or a body word names a word of the loop, which
     each copy would still name in the original. Only a straight-line body is
     copied.
   - `used`: a word of the loop may be written, so the copies would not see the
     change, or the program reads, executes or takes the address of the setup,
     the `dac`, the `isp` or the `jmp`, which are deleted.
   - `acentry`: the body reads AC before loading it. In the loop, the body's
     first word sees -n on the first pass and what the `isp` left on the
     others; unrolled, it sees what the copy before it left.
   - `acexit`: the code after the loop reads AC before loading it. The loop
     leaves +0 there; unrolled, AC holds whatever the last copy left. A `hlt`
     counts as a read, since Continue resumes with AC as it is.
2. **The declaration**, as above: `partial`.
3. **The spans.** A word of the loop in a `%%nooptimize` span is `handsoff`.
4. **The guess.** A loop any heuristic marks is `guessed`, **at every level**,
   not only under `-O2`. H2's delay loop is exactly this shape, and a delay
   loop unrolled is a shorter delay. H2 calls a loop a delay loop when every
   word it writes is one of its counters: a word an `isp` or `idx` in the loop
   steps. So a body that only counts with `idx` is left alone. Write
   `lac n; add one; dac n` if you mean it to be unrolled.
5. **No unroll touches another rewrite.** A loop with a word another loop, an
   inline copy or a move touches, or a word a T3 or T8 rewrites, is
   `overlap`. Bisection can then switch any unroll off on its own.
6. **The cap.** A loop whose body would become more than 64 words (n x B,
   `-O=unroll=cap:N`) is `cap`.
7. **The budget.** A loop that grows the program must fit in its bank's free
   words, less what the inline copies took and the inline reserve
   (`-O=inline=reserve:N`); one that does not is `budget`. The free words
   stop at a declared `%%ceiling`, below.

Every loop's line in `-O=xform` reads

```
xform fired S2 0 0210 prog.am1:32 head 0205 ha trips 3 body 3 spent 2 save 55 speed | isp ca => unroll 0205-0207 x3, delete 0203-0204 0210-0211
```

It gives the `isp`'s bank, address and line, the header and its label, the
trips, the body's length, the words spent, the microseconds saved over one
pass, and `speed` or `region`. After the `|` come the `isp` as written and the
edit. A loop not unrolled says why in the fate's place, a `refused` one adding
the judge's reason, a `guessed` one the heuristics and a `cap` one the cap.
The summary line gains `s2sites`, `s2fired`, a count per fate, `s2words` and
`s2us`, printed only when a loop was recorded. The report's Loop unrolling
section lists every loop with what happened to it.

The copies are relayout's, made as a copy is made over the `isp`, with the
`jmp` deleted in the same edit, since copies left with their `jmp` back would
loop. The deletes of the `dac` and the setup follow. A loop relayout refuses
stays as it was. A setup word relayout keeps loads AC and stores to the dead
counter, which the judge made sure nothing notices. The report and stderr say
so in both cases.

### Space mode: deleting a word

Under `-O1` or `-O2`, the peephole rules that save a word by deleting one are
made where you declared they may be:

| Rule | The words | Become |
|---|---|---|
| T1 | two operate words, `cla` then `cma` | one, `cla cma` |
| T1b | two shifts of one opcode, `ral 2s` then `ral 3s` | one, `ral 5s` |
| T2 | a skip, then `jmp` over the next word | the opposite skip: `sza` becomes `sza i`, `spa i` becomes `spa`, `sad x` becomes `sas x` and `sas x` `sad x` |
| T6 | `cla` before a word that loads AC outright (`lac`, `law`) | nothing |
| T7 | `cli` before a `lio` | nothing |
| T13 | `jmp` to the next word, written `.+1` or as its label | nothing |

T1, T1b and T2 keep their first word, rewritten, and delete the second; T6, T7
and T13 delete their one word. Every later word moves down, up to the next
origin, and every name, constant and pointer that named a moved word follows
it, as the other length-changing rewrites' do. A label on a deleted word moves
to the word after it. T14, a copy between AC and IO through a temporary, is
not made: it replaces two words with a PDP-1D instruction that a machine may
not have.

**What licenses it.** Every word of the pattern must be in an **optimize or a
speed region**, or `-O2` must be given `-O=undeclared`, which licenses a
deletion anywhere on the guess. Without the switch, `-O2` leaves an undeclared
deletion as advice, and its report says *a deletion, which -O2 makes only
inside an optimize or speed region*.

A licensed finding's fate is decided before any word moves, in this order:

1. **The spans.** A word of the pattern in a `%%nooptimize` span is `handsoff`.
2. **The guess.** A deletion shortens every pass through the code after it, so
   a pattern in a delay loop (H2) or a device loop (H3) is `timed`, **at every
   level**. Under `-O2` the finding is also asked what `-O2` asks of T3 and T8:
   H1 alone when the whole pattern is in optimize regions, all five when a word
   of it is in a speed region only or, under `-O=undeclared`, in no region.
   H6 is asked at every level, so a pattern
   in a run a computed address indexes is `guessed` at `-O1` too, as is a
   window deletion there. One any of those mark is `guessed`.
3. **The words.** The deleted word must be a whole statement of one word, and
   not one written on an origin's line, and the kept word must be one whose
   expression can be replaced: otherwise `unrepresentable`. So is a merge or
   an inversion am1 cannot spell -- a skip written as a number has no `i` to
   take out, and `500000+x` has no `sad` to turn into `sas`.
4. **No deletion touches another rewrite.** A pattern holding a word an earlier
   deletion, an inline copy, a move or an unroll touches, or a word a T3 or T8
   rewrites, is `overlap`, so bisection can switch any rewrite off on its own.
   `cla` before a `lac [5]` that T8d makes `law 5` is left, for example.

The kept word's new expression is built as the parser would build its
spelling, and checked: it must assemble to exactly the word the rule names, and
for T1 the merged word, simulated, must leave AC and IO as the two words did one
after the other. A merge or an inversion that fails either check is
`unrepresentable`, and its words stay as written.

The deletions are relayout's, one delete each, and a kept word's new
expression goes in with its delete, in the same edit. So when relayout refuses
a deletion -- because a number, an offset or a `.` would name a different word,
say -- the kept word stays as you wrote it too: a skip is never inverted
without its `jmp` gone. The warning says so:

```
am1: warning: space: relayout refused T6's delete at bank 0 0204 (number), so its words stay as written; prog.am1:96
```

A deletion's line in `-O=xform` reads

```
xform fired T2 0 0101 prog.am1:18 deletes 0 0102 keeps 0 0101 640100 650100 region 1 | sza; jmp .+0o2 => sza i assumes H2,H3
```

It gives the pattern's bank, address and line, the word deleted, the word kept
with its old and new values, and the region. After the `|` come the words as
written and what they become, `(deleted)` for T6, T7 and T13. A deletion not
made says why in the fate's place, `timed` and `guessed` adding the heuristics.
The summary line gains `timed`, `overlap`, `spacewords` and `spaceus`, printed
only when a licensed deletion was decided. With `-O=relayout`, a `space:` line
per deletion says whether relayout made it. The report's Space mode section lists
every licensed deleting finding with what happened to it, and counts the words
and microseconds the ones made saved.
A deletion only `-O=undeclared` licensed is marked `(undeclared)` there.
Bisection numbers the deletions after the unrolls.

On `adventure.am1`, with the regions `genregions.py` declares, `-O1` deletes
83 words, 60 by T2 and 22 by T13, and leaves 12 that are timed and 5 that
overlap another rewrite.

### The pool reclaim: a constant nothing names

T8a to T8d replace a `lac` or `lio` of a constant pool word with an instruction
that loads the value outright: `cla`, `cli`, `cla cma` or a `law`. The
instruction takes the same one word, so those rules change no address. What
they do change is the pool: once the last word that named a pool word has been
rewritten, the pool word holds a number nobody reads.

Where **a declaration licensed every rewrite that stopped naming it**, that
word is now dropped. The pool is a word shorter, everything after it moves
down, and every reference to the constants after it follows them, exactly as
after a deletion. A pool whose every word goes disappears, directive and all.

The license is the point, and it is narrower than it looks:

- **`-O1`** fires T8 only inside an `%%optimize` region, so every word it frees
  is licensed and every one is collected.
- **`-O2`** fires T8 anywhere. A word it frees inside a region is collected; a
  word it frees on the guess alone is **kept**. Collecting that one would turn
  a same-length rewrite, which the guess is allowed to make on its own, into
  one that moves every word after it -- and a length change happens only where
  you declared it may (ruling 1).
- **`-O2 -O=undeclared`** collects every word it frees. The switch is your
  word that a length change may happen wherever the guess allows one.
- The test is **every** reference, not any: a pool word that two `lac`s read,
  one of them in a region and one not, is freed by the pair and kept, because
  one of the two rewrites was licensed by nothing.

The reclaim is not a rewrite and **takes no bisection number of its own**. It
follows the rewrite that freed the word: switch that rewrite off with
`-O=upto`, `-O=range` or `-O=off` and the word stays, because something still
names it. `-O=relayout` prints a `reclaim:` line per word, naming the rule and
the address of the rewrite that freed it, and the report's **Pool reclaim**
section says how many of the licensed words went, lists each with the word it
collected, and -- ruling 1 again -- names the words freed with no declaration
behind them, which an optimize region over those rewrites, or `-O=undeclared`,
would collect.
A word only the switch licensed is marked `(undeclared)`.
The section is written whenever `-O1` or `-O2` made its rewrites, and says `0 of 0` when nothing was freed.

Relayout rebuilds the pool after moving code, and the rebuilt pool is not always the old one less the collected words.
A constant several references share, written as an address, can have different values once code has moved, and each value then takes a slot of its own.
Constants whose values have become equal can share one.
When either changes the pool's size, the section says so on a line of its own, `Relayout added N pool words, not counted above` or `Relayout dropped N more pool words than counted above`.
The bank table of `-S` counts them; the section's other figures do not.

On `adventure.am1`, with the regions `genregions.py` declares, both `-O1` and
`-O2` collect all 44 of the pool words the declared T8s free. `zmemcore.am1`,
which declares nothing, collects none: `-O2` frees 39 pool words there, and its
report names all 39 as words a region would buy.

### A bank's ceiling: `%%ceiling`

Inline expansion and loop unrolling grow a bank into its free words, and free
means no assembled word is there. A program that fills memory at run time --
a stack copied up from below, a buffer it builds, a table a device writes --
uses words no assembled word shows, and a copy that grew into them would be
overwritten by the program, or would overwrite it. `%%ceiling` says where
those words begin:

```
%%ceiling ZU_STACK_COPY & 07777
```

`EXPR` is the **first word no rewrite may reach**. It is an expression like
any other, reduced to the bank (so a full address does as well as its last
twelve bits), and it may name only what is already defined when the line is
read: a name defined later is an error, since the ceiling must not depend on
the layout it limits. It applies to the bank the line is in, wherever in the
bank the line is. It can only **lower** the default, which is 07751 in bank 0
(the read-in loader's first word) and the end of the bank elsewhere: 0, and a
value above 07751 in bank 0, are errors. A bank may declare more than one,
from several include files say, and keeps the lowest; the others are listed
as not kept.

Only the optimizer reads it. Plain assembly checks the expression and then
ignores the line, so a source that declares a ceiling assembles to the same
words with or without it, at every level, unless a copy or an unroll is
stopped by it. Under `-O`, `-O1` and `-O2`:

- the inline budget and the unroll budget count a bank's free words up to the
  word before its ceiling. A copy that no longer fits is `full` or `budget`, and its
  `-O=xform` line ends `declared ceiling N`; the report's refusal names the
  `%%ceiling`.
- a program whose highest assembled word in a bank is already at or above the
  bank's ceiling stops the assembly, naming the directive and the word: no
  rewrite made that, and the declaration or the program is wrong.
- relayout refuses an `-O=edits` edit that would reach a ceiling, `ceiling` in
  the `-O=relayout` dump. The transforms' own edits pass through states their
  budget never counted -- an unroll's copies land before its setup is deleted
  -- so they are checked once, after the last; a bank over its ceiling there
  is an internal error, which stops the assembly.

The report's Bank ceilings section, `-O=regions`' `ceiling` lines and
`-O=speed`'s budget lines show what was declared and what it left free.

## `-O1`: rewriting inside a region

```bash
am1 -b -d -O1 program.am1
```

`-O1` is `-O` plus one step: after the analysis, and before any output is
written, it rewrites some words. `-O2`, below, is the same step with no region
needed. There is no other level: `-O3` and above are refused, and so are
`-O2s` and `-O2t`. The space and speed modes they were once meant to name are
made by `-O1` and `-O2` themselves, inside a declaration: see "Declaring where
a length-changing rewrite may happen".

### What it rewrites

Anywhere in a region, five rules, each of which replaces one word with one word
at the same address:

| Rule | The word | Becomes |
|---|---|---|
| T3 | `jmp A`, where A holds `jmp B` | `jmp B` |
| T8a | `lio [0]` | `cli` |
| T8b | `lac [0]` | `cla` |
| T8c | `lac [777777]` | `cla cma` |
| T8d | `lac [n]`, n from 0 to 7777 | `law n` |
| T8d | `lac [~n]` | `law i n` |

By these five, nothing is inserted, nothing is deleted, no label moves and no
symbol changes, so every other word of the program is where it would have been
without the flag. The other rules change the program's length. T1, T1b, T2, T6,
T7 and T13 each delete a word, and space mode makes them inside a region too
(see "Space mode: deleting a word"). The rest of this section is about the five.
T14 stays advice, and so, permanently, does return-word sharing.

### Only where all of this holds

A word is rewritten only when **every** one of these is true:

1. A **live finding** names it: the pattern passed P1 to P4 and every refusal
   of its rule, `through-taken`, `unreached` and `table` included. A refused pattern
   is never rewritten, and the report goes on listing it as suppressed for its
   reason.
2. **No word of it is in a `%%nooptimize` span**, nor is the word a T3 reads
   through. The report puts such a finding down as *inside a nooptimize span*,
   before any other reason.
3. The **whole pattern is inside a region.** The report puts a pattern outside
   every region down as *outside every declared region*, and one partly inside
   as *straddling a region boundary*. Neither is rewritten.
4. The rule is **one of the five.** A deleting rule's finding is space mode's
   to decide, and T14's is put down as *a rule -O1 does not carry, because
   it changes the program's length*.
5. For T3, the chain of jumps has **a far end**. On a chain `jmp a`, `a: jmp b`,
   `b: jmp c`, `-O1` follows the jumps to the end, so `jmp a` becomes `jmp c`,
   and `a: jmp b` becomes `jmp c` too. A second `-O1` run then has nothing left
   to follow. Each further word of the chain must be a direct `jmp` that no
   refusal of T3 would stop: not written, patched or taken as a value. The chain
   ends at the first word that is not one, and the jump lands on it. It also
   ends, landing on the word, at a word in a `%%nooptimize` span and, under
   `-O2`, at a word the guess marks. A chain that comes back on itself is put
   down as *a chain of jmps that comes back on itself, so it has no far
   target*. One longer than 4096 jumps is put down as *a chain of jmps longer
   than the optimizer follows*. Neither fires, and a long chain is never cut
   short.

So what `-O1` rewrites is always a subset of what `-O` already reports, and the
report says which subset and why each of the rest was not rewritten.

### How the new word is written

The rewrite replaces the word's expression in the parse tree with the one the
parser would have built from the replacement text, so the listing and the
macro1 source read as if you had written it:

- T3 keeps the far target's **name** when the intermediate jump named a plain
  global label (`jmp enda`). Anything else becomes the address as a number
  (`jmp 106`): a local label would name the wrong word, or none, in the
  rewritten jump's scope, and a `.` would mean an address one statement away.
- T8d writes the constant's value as a number: `lac [dwarfLoc:0]` becomes
  `law 6254`. When an inline copy or a placement then moves words, the number
  moves with them. A T3 jump written as a number is set to where its target
  word now is. A T8d whose constant names anything but literals is set to the
  value the constant has after the move. If the new value no longer fits the
  `law`, the edit is refused `number`.
- T8c under `-a`, where a space means add, is written `cla|cma`. `cla cma` read
  that way is 760200 plus 761000, which is not an operate word at all.
- A constant reference whose closing bracket you left out keeps its line.
  With a comment after it (`lac [6  // six`), the listing and the macro1
  source show `law 6` and the comment, on one line. Without one (`lac [5`),
  they show `law 5` on a line of its own, as if you had typed it.

A pool word that no instruction names any more is **freed**. Where a
declaration licensed every rewrite that stopped naming it, relayout removes it,
and every word after it moves (see "The pool reclaim: a constant nothing
names"). A word freed on `-O2`'s guess alone is still emitted, at the same
address, holding the same value, because removing it would move every word
after it. The report counts both.

### The checks it makes on itself

Each rewrite is checked three ways as it is made:

- the rule must not change the program's length;
- the new expression must assemble to exactly the encoding the rule names;
- that word, decoded afresh, must put the pool word's value, or the far
  target, where the original word did, with the other register untouched.

A failure of any of them is an internal error in **am1**, and it stops the
assembly before a single output file is written, the `.opt` included. It looks
like this, from a deliberately broken build:

```
am1: internal error in -O1: T8b at bank 0 0102 built a word that assembles to 760200, not the 764000 the rule names
at line 21, file t8b_test.am1
```

None of them is expected to fire. If one ever does, the program is not
assembled at all, and the message is the thing to report.

### What it reports

The header of an `-O1` report says it is also the record of what `-O1`
changed, and a Transforms section follows the Optimize regions section. From
a small test source, `fates_test.am1`, after the section's opening
paragraph:

```
  Rewritten: 1
    T8d  bank  0 0101  fates_test.am1:38  region 1
        before 200115  lac [0o7]
        after  700007  law 0o7

  Not rewritten: 3
    T8d  bank  0 0100  fates_test.am1:36  outside every declared region
    T14  bank  0 0103  fates_test.am1:40  a rule -O1 does not carry, because it changes the program's length
    T1   bank  0 0106  fates_test.am1:43  straddling a region boundary
```

After the lists it says what the rewrites realized: the code words space
mode's deletions remove; the storage words freed, how many of them inside a
declaration, which the Pool reclaim section says went; microseconds from one pass through the rewritten words, 5 per
T8; and, separately, the rewritten jumps, which save a cycle only on a hop
actually taken. If the evidence disputes any region, the warning is repeated
there, with the rewritten words that sit in each disputed region.

Everything else in the report describes the program **as written**: the
findings, their words and their evidence are the analysis of the source
before any rewrite. The Transforms section is the one place a rewritten word
appears.

`-O=xform` prints the same record on stdout, one line per live finding. With
`-O1`, **am1test** (see "The testing build") also analyzes the rewritten
program a second time and prints what a second `-O1` would do, which must be
nothing (`second: dryrun fired 0`). The installed **am1** does not: the pass
is a self-test, and it doubled the analysis of every such build.
Without `-O1` it is a dry run: it says what `-O1` would do with the five rules
above and changes nothing, not even the report. A deletion, a copy, a move and
an unroll are decided only under a level, so the dry run lists a deleting
finding as not carried and has no `S1`, `S2` or `S4` lines; give the level you
build with to see them. A rewritten T3 that followed more than one hop ends
`via` and the words it jumped through, and one whose chain ended at a span or
a guess says `stopped`, the word, and `handsoff` or `guessed`.

### What stays yours

`-O1` makes the words it rewrites faster. A T8 drops a memory cycle, and a T3
drops a jump on every pass through it. That is why a region declares that none of
its code is there to take the time it takes, and why that is one of the two
things the advisor cannot check. Keep delay loops, device timing and anything
counted in microseconds out of your regions.

Before you trust a rewritten program, assemble it twice, with and without
`-O1`, and run both. On `adventure.am1`, with a region over every span the
contradiction check reports clean, `-O1` rewrote 134 words when this was
measured: 126 T8d, 6 T3 and 2 T8a. Adventure's 78-test suite, run against
both builds, reached the same pass/fail set. The regions now
license the length-changing rewrites too, so the same build also places code
and deletes words; each of those sections gives its own count.

## `-O2`: rewriting on a guess

```bash
am1 -b -d -O2 program.am1
```

`-O2` makes `-O1`'s five rewrites, under `-O1`'s other conditions, with no
region needed. A region is the author's answer to P5; `-O2` guesses the answer
instead, and leaves alone any finding the guess says might be shared with a
device, a sequence-break handler or a timing requirement.

**The guess can be wrong**, and every `-O2` assembly says so first, on stderr,
whether or not `-W` is given:

```
am1: warning: -O2 guesses where code is shared with a device, a sequence-break
am1: warning: handler or a timing requirement, and the guess can be wrong.  Test
am1: warning: the program.  program.opt names the guess behind every rewrite, and
am1: warning: %%nooptimize/%%endnooptimize keeps -O2 out of code it must not touch.
```

The assembly still succeeds. The warning is about the assembler, not the
source.

### The guess

Six heuristics make it. Each marks words, and a finding is left alone when a
heuristic marks any word of its **footprint**: the words of its pattern, plus
the pool word a T8 loads or the intermediate jump a T3 reads through.

| Id | Marks | The rewrite assumes |
|---|---|---|
| H1 | handler territory: every block a sequence-break entry reaches, calls included, and every word such a block writes | no sequence-break handler reaches or writes it |
| H2 | a delay loop: a cycle that writes nothing but its own counters, with no call and no in-out transfer | not in a delay loop |
| H3 | a device loop: a cycle that holds an in-out transfer to a device, or executes with `xct` a word the program writes | not in a loop that transfers to a device |
| H4 | a device buffer: a word whose address is loaded where an in-out transfer runs | not a device buffer |
| H5 | an absolute address: a memory reference written as a number, and the word it names | no address involved is written as a number |
| H6 | a computed address: a run of code that an address changed at run time indexes, from that address to the next label | no address changed at run time indexes the words it moves |

**H6 is different from the other five.** It is asked only of a rewrite that
changes the program's length, such as a deletion, a window deletion, a
placement or an inline copy, and it is asked at **every** level, `-O1`
included, inside a region or not. A rewrite of one word for one word, T3 or
T8, never asks it.

H6 exists for code like this:

```
        law tbl
        add ix          // the entry, computed at run time
        dap ent
ent,    jmp .
tbl,    law 1           // entry 0
        cla             // entry 1
        cli             // entry 2
        ...
```

Merging the `cla` and `cli` would take a word out of the table and move
every entry after it. The table is code, not data, and only `tbl` itself is
taken, so nothing else refuses the merge. H6 looks for an address loaded
with `law` (or from a word holding it), then changed in the same block by an
`add` or `sub` of a variable or a constant, or stored into a pointer that an
`idx` or `isp` walks. It marks the run from that address to the next label.
It does not see arithmetic done in another block, or an entry past the run's
first label: use a `%%nooptimize` span for those. `-O1` reports a finding
it holds as `left alone on H6's guess`.
A **cycle** is one loop header and one latch, the jump back to it, with the
blocks between. So a spin wait that jumps back to the top of a main loop is
seen as a delay even though the main loop as a whole writes state. Extend mode
(`eem`, `lem`) and the sequence-break controls are not devices. A halt's edge
to the next word is not followed, because going round it takes someone
pressing Continue. H1 needs the
program to hold an enable command, as the frames do (see "What the advisor
cannot know").

`-O=guess` prints everything the heuristics marked, and the report's What -O2
would assume section counts and explains it. Both exist without `-O2`, so you
can read the guess on a program before you let `-O2` act on it.

### Regions and spans under `-O2`

A region under `-O2` is not a permission, since none is needed. It is a
narrower guess. Inside a region your declaration stands in for H2 to H5, and
only H1 still applies: a region says nothing is there to take time or shares
memory with a device, but it does not say no handler reaches it. H6 applies
too, to a length change, as it does everywhere. A finding only partly inside
a region gets them all, as one outside every region does.

A span is absolute at every level. Use one when the guess misses something:
a delay built from something the heuristics do not recognize, a device word
reached through a pointer, or anything else you know.

### What it reports

The `-O2` report opens with the warning again, in its header, and says that it
is also the record of what `-O2` changed. P5's sentence says the heuristics
guessed it, and that inside a region your declaration stands in for all but H1.
Each finding's `region:` line says which heuristics `-O2` applied there. The
Transforms section is titled `Transforms (-O2)`, begins its Rewritten list with
what each heuristic assumes, and follows every rewritten word with the
heuristics it assumed. From a test source, `heur_region_test.am1`:

```
  Rewritten: 2
  Each rewrite rests on the guess that no heuristic it names found anything:
    H1  no sequence-break handler reaches or writes it
    H2  not in a delay loop
    H3  not in a loop that transfers to a device
    H4  not a device buffer
    H5  no address involved is written as a number
    H6  no address changed at run time indexes the words it moves
    T8d  bank  0 0101  heur_region_test.am1:56  region 1
        before 200600  lac [0o5]
        after  700005  law 0o5
        assumes: H1
    T8d  bank  0 0160  heur_region_test.am1:88  region 3
        before 200604  lac [0o11]
        after  700011  law 0o11
        assumes: H1

  Not rewritten: 4
    T8d  bank  0 0120  heur_region_test.am1:66  left alone on -O2's guess: H3
    T1   bank  0 0142  heur_region_test.am1:78  left alone on -O2's guess: H2
    T8d  bank  0 0162  heur_region_test.am1:91  inside a nooptimize span
    T8d  bank  0 0200  heur_region_test.am1:100  left alone on -O2's guess: H1
```

The word at 0101 is in a flag poll that H3 marks. It was rewritten because it
is inside a region, where only H1 applies. Its twin at 0120 is outside every
region and was left alone. A rewrite outside every region says `no region`
where these say `region 1`, and assumes all five.

`-O=xform` under `-O2` prints the same record: a rewritten line ends
`assumes H1,H2,H3,H4,H5` or whichever applied (a length change adds `H6`), a
left-alone line `by` the heuristics that marked it, and a line in a span
`handsoff`.

### What stays yours

`-O2` is for programs you will test. On `adventure.am1`, with no region, it
rewrites 132 words (124 T8d, 6 T3 and 2 T8a) and puts 15 findings down to its
guess: 13 in device loops and 2 in delay loops, among them the delay routine at
bank 2 7034. The guess is applied before the rule is asked about, so some of
the 15 are rules no level carries yet. Every word it rewrites is one `-O1` could rewrite inside a region, so
the checks it makes on itself are `-O1`'s, and a second pass over the
rewritten program rewrites nothing. Adventure's 78-test suite, run against
the `-O2` build and the stock one, reached the same pass/fail set. That is one program. Run yours, and when it breaks, the next section
finds the rewrite that did it.

### `-O=undeclared`: shortening the program on the guess

```bash
am1 -b -d -O2 -O=undeclared program.am1
```

Plain `-O2` makes only the rewrites that leave every word where it was.
`-O=undeclared` lets the same guess license the rewrites that shorten the program, where no region declares them:

- space mode's deletions, T1, T1b, T2, T6, T7 and T13 (see "Space mode: deleting a word");
- the pool reclaim, the pool words those rewrites and T8 stop naming (see "The pool reclaim: a constant nothing names");
- the extend window's deletions, a redundant `eem` or `lem`, a dead `lem` and the `eem` it frees (see "Deleting `eem` and `lem`");
- fall-through placement (see "Fall-through placement").

Each is made wherever a region would have let it be made, less what the guess refuses.
Outside a region every heuristic applies, H1 to H6; inside one, what applied before still applies.
A deletion in a delay or device loop is still `timed`, and a `%%nooptimize` span still keeps every level out.
Inline expansion and loop unrolling add words, within a budget you set, and still need their declarations: the switch does not reach them.

It is refused without `-O2`:

```
am1: -O=undeclared lets -O2's guess license the length-changing rewrites, and needs -O2
```

With it, stderr gains three lines after `-O2`'s four:

```
am1: warning: -O=undeclared deletes and moves code outside every region, on the guess.
am1: warning: It does not see arithmetic on an address outside the block that loads it,
am1: warning: nor an entry past the first label of the run an address indexes.
```

Those are the two things H6 does not see (see "The guess").
A table whose address is loaded in one block and advanced in another is the first.
A computed jump that lands past a label inside the table is the second.
A deletion or a move in either place shifts the entry the program computes, and nothing in the assembly says so.
Put a `%%nooptimize` span around such code before you use the switch.

The report says `licensed` where plain `-O2`'s says `declared`, and marks each rewrite only the switch licensed `(undeclared)`, in the Space mode, Pool reclaim, Extend window deletions and Fall-through placement sections.
A rewrite a region licensed carries no mark.
Bisection numbers each of them as it would a declared one, and a pool word follows the rewrite that freed it, so the next section finds a bad one the same way.

`-O=place=undeclared`, the first spelling, once licensed placement alone.
It is now the same switch, and stderr and the report name the spelling you gave.

On `adventure.am1`, with no region, `-O2 -O=undeclared` saves 246 words: 75 deletions, 43 pool words, 118 window words and 10 moves.
`genregions.py`'s 441 regions under `-O1` save 245.
The two differ in detail.
The switch asks all six heuristics where a region asks H1 alone, so it leaves a few words in device loops that the regions take.
It also reaches the included library files, which the regions never wrap.
Test the program you build with it.

## Finding the rewrite that broke a build

When a build under `-O1` or `-O2` misbehaves and the stock build does not, one
of the rewrites did it. Three modifiers switch rewrites off, so you can find
which one without editing the source:

| Modifier | Keeps on |
|---|---|
| `-O=upto=N` | the first `N` rewrites, in the order `-O=xform` prints them; `N` is decimal, and `-O=upto=0` keeps none |
| `-O=range=B:LO-HI` | the rewrites in bank `B` (decimal, as the dumps print a bank) at addresses `LO` to `HI` (octal). Give it more than once and the ranges add |
| `-O=off=FILE` | every rewrite except those `FILE` lists. Give it more than once and the lists add |

They apply only with `-O1` or `-O2`. Given without a level, **am1** prints its
usage and stops. With more than one, a rewrite stays on only if **every**
modifier keeps it: `-O=range` narrows the search to a bank, `-O=upto` halves
inside it, and `-O=off` always wins.

**Why a partial build is a valid program.** **am1** decides what happens to
every finding before it rewrites any word, and it decides from the program as
written. So switching one rewrite off changes no other rewrite. That holds even
for a jump chain whose intermediate is itself switched off: the first link
still jumps to the far target it was given. Every T3 and T8 rewrite is one
word for one word at the same address, so nothing moves. Each word of a
bisected build is either the stock word or the word the full build writes
there.

**Inline expansion is bisected the same way.** The copies are numbered after
the T3s and T8s, in address order. The key for `-O=range` and `-O=off` is the
call site, and a switched-off site stays a call. Copies do move words, so a
bisected build's words are not simply the stock and full builds' words
interleaved. The argument still holds: every copy's fate is decided before
anything moves, and no copy holds another (the `nested` fate). Any subset is
the program you would get by copying just those routines by hand. A routine
deleted because its only call was copied is kept when that copy is switched
off.

**So is fall-through placement.** The moves are numbered after the copies, in
address order, and the key is the deleted `jmp`'s address. A switched-off
site keeps its `jmp` and its run where they were. No move touches another
move, a copy or a rewritten word (the `overlap` fate), so, again, any subset
is the program you would get by moving just those runs by hand.

**And loop unrolling.** The loops are numbered after the moves, in address
order, and the key is the loop's `isp`, a word no other rewrite makes. A
switched-off loop stays as written. No unroll touches another rewrite (the
`overlap` fate), so any subset is the program you would get by unrolling just
those loops by hand.

**The list file.** One rewrite per line, `bank addr`: exactly the fourth and
fifth fields of an `-O=xform` line, so a line can be copied out of the dump.
`#` starts a comment, and a blank line is ignored:

```
# the rewrites that broke the demo
0 1630          # T8d, adventure.am1:1931
2 7040
```

A line in another shape stops **am1** with the file and line named, before
anything is written. **A listed address that names no rewrite prints a
warning, and the build goes on:**

```
am1: warning: -O=off=bad.off, line 3: bank 2 7040 names no rewrite that would fire; is the list stale?
```

That happens when the source was edited and the addresses moved. The key is
the address as assembled from the source, so a list goes stale on any edit
that moves code, however harmless.

### What the dumps and the report say

With any of the three modifiers, `-O=xform`'s summary line ends its fates with
`off N`. Every rewritten line ends `#n`, its ordinal, which is the `N` that
`-O=upto` counts, and every switched-off line says what switched it off:

```
xform: applied fired 5 ... off 1 freed 2 us 20 hops 2 hopus 10 ...
xform fired T3 0 0111 bisect_test.am1:51 600113 600117 through 0 0113 clean region 1 | jmp a => jmp c via 0113 0115 #5
xform off T3 0 0113 bisect_test.am1:53 by upto #6
```

In **am1test**, the second pass `-O=xform` runs over the rewritten program
then fires exactly the switched-off rewrites. The source still holds those words as written.

The report's Transforms section opens with a paragraph that gives the
modifiers, how many rewrites they switched off and by which modifier. It lists
each switched-off rewrite under Not rewritten:

```
  Bisection (-O=off=bad.off): 2 of the 6 rewrites that would fire
  were switched off: 0 by -O=upto, 0 by -O=range, 2 by -O=off.  Each is listed
  under Not rewritten, and every word it names is the word the source wrote.
  ...
    T8b  bank  0 0103  bisect_test.am1:45  switched off for bisection: -O=off, rewrite #2
```

When the source also has inline sites, the counts include them, and the last
line says how many are listed in each place. A switched-off copy appears
under Inline expansion as `not copied`:

```
  Bisection (-O=range=0:106-107): 3 of the 5 rewrites that would fire
  were switched off: 0 by -O=upto, 3 by -O=range, 0 by -O=off.  Each is listed
  under Not rewritten (1) or Inline expansion (2), and every word it names is
  the word the source wrote.
```

The storage-freed figure counts only the rewrites left on. A switched-off
`lac` still reads its pool word, so that word is not free. `-O2`'s warning on
stderr gains one line:

```
am1: warning: bisection: 127 of the 132 rewrites -O2 would make are switched off (-O=upto=5).
```

Without a modifier, nothing in any output changes.

### The driver: `am1bisect.sh`

Halving `N` by hand takes about eight builds for Adventure's 132 rewrites, and
a hand-edited number is easy to get wrong. `Tools/AM1/am1bisect.sh` does it:

```bash
am1bisect.sh [-a am1] -t 'test command' [am1 flags...] -O1|-O2 [am1 flags...] source.am1
```

It builds in the current directory, exactly as **am1** would by hand, and after
each build runs the test command with `bash -c`. Exit status 0 means good,
anything else bad. It checks two things before it starts. The build with
every rewrite on must fail, or there is nothing to find. The build with
`-O=upto=0` must pass, or the failure is not a rewrite's. Then it halves
`-O=upto` until one rewrite is left, and prints that rewrite's `-O=xform` line.
It writes `source.bisect.off` naming the rewrite, rebuilds with that list, and
runs the test once more:

```
every rewrite on: 6 rewritten of 6 that would fire
                  bad, as it must be
no rewrite on:    good, as it must be
-O=upto=3: bad
-O=upto=1: good
-O=upto=2: good

The rewrite that breaks the build is #3 of 6, found in 6 builds:
    xform fired T8c 0 0105 bisect_test.am1:47 200123 761200 through 0 0123 clean region 1 | lac [0o777777] => cla cma #3
Written to bisect_test.bisect.off.
The build with -O=off=bisect_test.bisect.off passes the test, and is the build now in this
directory.
```

It assumes **one** bad rewrite. If there are two, halving by count finds the
earlier one, and the last test still fails. The script says so and exits 1.
Rename the `.off` file, add `-O=off=` with it to the **am1** flags, and run the
script again to find the next. The other `-O=off` and `-O=range` flags you give
narrow what it searches. It exits 2 when it refuses to start or a build does not
assemble.

A test command can be anything that tells good from bad: an emulator run of
your own test, a check on the test dump (`-T`), or a person answering. Keep it
deterministic. A flaky test gives a wrong answer with a straight face.

## Testing relayout: `-O=edits=FILE` and `-O=relayout`

*Both modifiers are **am1test**'s; the installed **am1** refuses them (see
"The testing build").*

Every T3 and T8 rewrite `-O1` and `-O2` make replaces one word with one word at
the same address. A rewrite that deletes a word, moves a run of words, or copies a run
over a word, changes the address of everything after it, and **am1** fixed
every address while it parsed. Relayout is the machinery that lays the program out again after such
an edit. It recomputes each word's address, each label, each `.`, each
resumed bank, the constant pools, the variables and `start`. It refuses any
edit it cannot show leaves every reference naming the word it named.

**Every rewrite that changes a program's length uses relayout**: inline
expansion, fall-through placement, loop unrolling, space mode's deletions, the
`eem` and `lem` deletions and the pool reclaim, each only where a declaration
licenses it (see their sections). Their copies, moves and deletes are edits
like the ones below, made after any `-O=edits` edits. `-O=relayout` prints an
inline expansion's as `inline:` lines, `copy` or `delete` with the site's
source line, and a delete whose copy was refused is `skipped`; it prints the
others as `place:`, `unroll:`, `space:`, `window:` and `reclaim:` lines. The
rest of
this section is the testing instrument that makes edits by hand, so the
machinery can be held to plain **am1**:

| Modifier | Does |
|---|---|
| `-O=edits=FILE` | makes the edits `FILE` lists, in order, and lays the program out again. Give it more than once and the lists add |
| `-O=relayout` | prints what relayout did on stdout; given alone, relayout runs with no edit and must change nothing |

Each line of the file is one edit. The bank is decimal and the addresses are
octal, as every dump prints them, and every address is the one the source
assembled to before any edit. `#` starts a comment:

```
delete 0 0102                   # the one word of the statement at 0102
move 0 0103 0105 after 0110     # the statements emitting 0103-0105, after the one ending at 0110
copy 0 0103 0105 over 0120      # the same statements again, in place of the one word at 0120
```

A deleted word's label moves on to the next word. A moved run takes its labels
with it. A copy deletes the one word at `ADDR` and puts a copy of the run
there, so a label on the deleted word comes to label the copy's first word:
the shape an inline expansion needs, where the call is the word that goes and
the callee's body is the run that replaces it. The original run stays where it
was, and the copy's words are new words: a run that defines a label cannot be
copied, since the label would be defined twice, and neither can a run that
names one of its own words, since the copy would still name the original's.
The word copied over must be an ordinary one-word statement.

In the listing, a deleted line keeps its source line with empty address and
value columns. A moved line stays where the source wrote it and shows its new
address, so the listing's addresses are no longer in order. A copied run is
listed twice, each time on the source line it was copied from, the second time
with the addresses the copy's own words have.

An edit is refused, and changes nothing, for one of these reasons:

| Reason | The edit |
|---|---|
| `noword` | names an address where nothing was emitted |
| `notstatement` | names a word that is not a statement's own, such as a variable's or a pool word |
| `nextword` | deletes a labeled word with no word after it before an origin |
| `unit` | moves a run that holds a directive: an origin, a bank, `constants`, `variables`, a region or span, an import or export |
| `segment` | moves a run past an origin or a bank directive |
| `region` | moves a run into or out of an `%%optimize` region |
| `conflict` | overlaps an edit already accepted |
| `overlay` | moves an address that holds two words (`-M`) |
| `offset` | would make an address plus an offset, such as `tbl+2`, name a different word |
| `dot` | would do the same with `.`, such as `jmp .+2` |
| `number` | would make a memory reference written as a number, such as `jmp 103`, name a different word; would leave a T8d `law` unable to hold its constant's new value; or copies a T3 or T8d word whose number relayout re-points, since it would not know the copy's |
| `label` | would leave a label on a different word than the one it named |
| `bound` | would push a word past the end of its bank |
| `overlap` | would put two words at one address |
| `pool` | would make a constant pool appear or vanish, or changes a constant it cannot key again |
| `copylabel` | copies a run that defines a label, which the copy would define a second time |
| `copyinternal` | copies a run that holds a memory reference naming a word of the run, which the copy would still name in the original |
| `internal` | relayout's own result is inconsistent; not expected, and **am1** stops |

**am1** warns once on stderr when any edit is refused. Every refusal is named
in the dump:

```
relayout: edits 1 accepted 1 refused 0 splits 0 dropped 0 grown 0 copied 0 unkeyed 0 heldorigins 0 heldtables 0 idempotent yes
segment: bank 0 from 0100 origin line 7
segment: bank 0 from 0200 origin line 18
edit: line 2 delete 0 0102 accepted
map: 0 0102 deleted
map: 0 0103 0102
...
reconcile: bank 0 before 13 deleted 1 grown 0 copied 0 after 12
```

`splits` counts the pool slots the rebuilt pools gained, and `dropped` the
ones they lost. **am1** shares a pool slot between constants that were equal
when parsed. An edit can make two of them differ, and then each needs its own
slot. `copied` counts the words the accepted copies added. `heldorigins` and
`heldtables` count origins and table counts whose expression named a symbol or
`.`. They keep the value the parser gave them, since relayout cannot tell
whether the author meant them to move. `idempotent yes` says that a second
layout changed nothing.

A copy prints one line more, saying where the copy went and how long it is:

```
relayout: edits 1 accepted 1 refused 0 splits 0 dropped 0 grown 0 copied 2 unkeyed 0 heldorigins 0 heldtables 0 idempotent yes
segment: bank 0 from 0100 origin line 11
edit: line 2 copy 0 0104 0105 over 0101 accepted
copy: bank 0 line 2 over 0101 words 2 at 0101
map: 0 0101 deleted
map: 0 0102 0103
...
reconcile: bank 0 before 10 deleted 1 grown 0 copied 2 after 11
```

The word at 0101 is gone and the copy of the two words at 0104-0105 stands in
its place, so everything after moves down one. The map is the map of the words
the program already had; the copy's own words are new and are not in it, and
the `copy:` line's `at` is where they begin.

## A worked example

```
A worked example.
// Three findings and one refusal, in as few words as possible.

bank 0

0200/
ex,     cla                     // T6: lac fills AC anyway
        lac count
        sza                     // T2: skip, jmp .+2, W
        jmp .+2
        dzm count
        lac [0o144]             // T8d: the pool word fits in a law
        dac count
        jmp .

0220/
        dap patch               // P2: this makes the cla below untouchable
patch,  cla
        lac count
        jmp .

0300/
        var count
        variables

    start ex
```

`am1 -S -O example.am1` writes `example.opt`. Its findings section:

```
Findings: 3
  Taken together they would remove 2 words and 1 storage word, and 15 microseconds from one pass through every one of them.
  Per rule: T2 1 T6 1 T8d 1

  T2   skip; jmp .+2; W is the inverted skip and W (1)
    bank  0 0202  example.am1:9  saves 1 word, 5 us
        0202 640100  sza
        0203 600205  jmp .+0o2
        0204 340300  dzm count
        becomes: sza i
        note: drop the jmp at 0203 and reverse the skip; the word at 0204 stays where it is in the source

  T6   a cla before an instruction that loads AC outright (1)
    bank  0 0200  example.am1:7  saves 1 word, 5 us
        0200 760200  cla
        0201 200300  lac count
        becomes: lac count
        note: lac count fills AC on its own, so the cla at 0200 changes nothing

  T8d  lac of a pool word holding a 12 bit value is a law immediate (1)
    bank  0 0205  example.am1:12  saves 0 words, 1 storage word, 5 us
        0205 200301  lac [0o144]
        becomes: law 0o144
        note: the pool word at 0301 holds 000144 and nothing else names it, so the pool loses a word too
```

The second `cla`, at 0221, matches T6 just as the first one does. It is not in
the findings; it is in the section below them:

```
Suppressed findings: 1
  These patterns matched and were refused.  Each line says which
  precondition, or which fact about the words themselves, blocked it.
  Per reason: P2-written 1

  T6   a cla before an instruction that loads AC outright (1)
    bank  0 0221  example.am1:18  P2: a word of the pattern is written while the program runs
        0221 760200  cla
        0222 200300  lac count
        would become: lac count
        because the address field of 0221 is patched by the dap patch at bank 0 0220, line 17
```

The suppressed half is often the more useful one. It does not say "nothing to
do here"; it says a rewrite that would otherwise be right is blocked, and by
what. If the `dap` were a leftover from a version of the routine that no
longer patches anything, that entry is how you would find out.

## The report, section by section

The `.opt` file has ten numbered sections, always in this order; 3a directly
after section 3 when the program reaches an `eem` or `lem`; and up to
five lettered ones directly after section 4: 4a when the source declares a
`%%nooptimize` span, 4a-speed when it declares a `%%speed` region or an `%%inline`
marking, 4a-ceiling when it declares a `%%ceiling`, 4b in every report, and
4c under `-O1` or `-O2`. Nine of
the ten are in every report; the seventh, recursion, appears only when there
is something to say. Up to six more come last, after the statistics, under
`-O1` or `-O2`: Inline expansion when the source declares a `%%speed` region or
an `%%inline` marking, then Fall-through placement when a placement site was
recorded, then Loop unrolling when a loop was, then Space mode when a licensed
deleting finding was decided, then Extend window deletions when a licensed
`eem` or `lem` was, then Pool reclaim (4d) whenever the level made its
rewrites. (Space mode was missing from this list since 1.22, and Pool reclaim
since 1.32; both are added here.)

**1. Header.** The program name, the **am1** version and date, the source file
name, the statement that nothing was changed (under `-O1` or `-O2`, instead,
that the report is also the record of what that level changed), the five
preconditions, and the P5 caveat in full, and a paragraph on sequence breaks.
Under `-O2` the header opens with a WARNING paragraph: the program was
rewritten on a guess, test it. The header is the
same in every report except for two things. One is P5's sentence. A source that
declares no `%%optimize` region gets "P5 is not checked here and must be reviewed
by hand", as every report always did; a source that declares one is told
instead that P5 is its own declaration and that section 4 checks it. Under
`-O2` it is told that P5 is guessed by the heuristics, and that inside a region
the declaration stands in for all but H1. The other
is the sequence-break paragraph, which says which frames the analysis assumed
and why (see "What the advisor cannot know"). A program with no `esm`, `asc` or
`isb` is told that no frame was assumed, and whose job a system already on is;
a program with one is told which enable command came first and which frames its
start address leaves; a program that enables breaks but starts below 4 gets a
warning instead. It is there so a `.opt` file handed to somebody else explains
itself.

**2. Findings.** The suggestions, grouped by rule, and within a rule sorted by
bank and then address. The order is fixed, so two reports for two versions of
the same program diff cleanly. Each finding gives:

- the bank and address, and the source file and line
- the saving, in words, in pool or storage words, and in microseconds
- every word of the pattern: address, octal value, and the word as the source
  spelled it
- `becomes:` the suggested replacement, in **am1** syntax
- `note:` why the rewrite holds, naming the specific addresses involved
- `region:` where the pattern sits with respect to the declared regions —
  printed only when the source declares at least one, and only in this list,
  never on a refused pattern in section 3

Numbers inside a suggested source line carry an explicit `0o` prefix. That is
not decoration: **am1** reads a bare number in whatever radix is current where
it lands, so a suggestion containing a bare `144` would assemble to something
else if it were pasted under `decimal`.

The group total says the saving is "from one pass through every one of them".
The advisor does no loop weighting. A word inside a loop that runs ten
thousand times counts once here, the same as a word in the startup path, so
the microsecond totals compare two versions of a routine rather than predict a
run time.

**3. Suppressed findings.** Every pattern that matched and was refused, with
the precondition or the fact that blocked it, and the evidence: which word,
which writer, which label, at which line.

**3a. Extend window, only when the program reaches an `eem` or `lem`, or
has an unexamined reference.**
Advisory: nothing in it is rewritten outside a declaration; the words inside
one that `-O1` and `-O2` delete are in Extend window deletions. A paragraph says what the
classes and hazards mean; then the counts of `eem`s, `lem`s, hazards and
reverse hazards, and of unexamined references when there are any; the
refusals by reason; and one line per redundant, freed or dead word, per proved
or unproved hazard or reverse hazard and per unexamined reference, with its
bank, address, file and line. See "The
extend window: `eem` and `lem`". `-O=window` is the working.

**4. Optimize regions.** Where the source has declared that `-O1` may
rewrite, and whether the evidence agrees with that declaration. On a
source that declares no region it is four lines saying so, and saying that
nothing above has been withheld for want of one — which is the state of every
program written so far. On a source that declares one it gives each region's
extent, word count and lines; how many words are inside a region in each bank;
how the live findings split between inside, outside and straddling; and every
word inside a region that the reference analysis saw written, patched or with
its address taken. See "Declaring where a rewrite is allowed" above for
what the section is for. `-O=regions` is the working.

Under `-O2` the section says what a region means at that level, and each
finding's `region:` line in section 2 says which heuristics `-O2` applied to
it.

**4a. Hands-off spans, only when the source declares one.** Each span's
extent, word count, lines and the live findings in it. See "Keeping every
level out" above. `-O=regions` prints the spans after the regions.

**4a-speed. Speed declarations, only when the source declares one.** Each
`%%speed` region's extent, word count and lines, the words declared per bank, and
each `%%inline` marking with the routine it names -- its entry, its body's word
count and how many sites call it -- or the note that it names no routine, or
that an earlier marking named the same one. See "Declaring where a
length-changing rewrite may happen" above. The section says what `-O1` and
`-O2` do with it and where the sections that follow report that. `-O=regions`
prints these after the hands-off spans.

**4a-ceiling. Bank ceilings, only when the source declares one.** A paragraph
on what a ceiling is, then one line per `%%ceiling` -- its bank, value, file
and line, and `kept` or `not kept, a lower one is` -- and one line per bank
that declares one: its highest assembled word, the last word a rewrite may
use, and the words free between them. See "A bank's ceiling" above.
`-O=regions` prints the `ceiling` lines after the speed declarations.

**4b. What -O2 would assume.** In every report, at every level: the five
heuristics, how many live findings each would refuse, and one line per refused
finding naming the
heuristic and the evidence, such as the loop and the in-out transfer in it.
The counts are the guess alone, before regions, spans or rules are
considered. See "`-O2`: rewriting on a guess" above. `-O=guess` is the working.

**4c. Transforms, under `-O1` or `-O2`.** Every word the level rewrote, with
both words in octal and as source (under `-O2`, with the heuristics each
rewrite assumed); every live finding it did not rewrite, with the reason in one
phrase; what the rewrites realized; and the region warning again when the
evidence disputes a region. See "`-O1`: rewriting inside a region" above.
`-O=xform` is the working.

**4d. Pool reclaim.** Present whenever `-O1` or `-O2` made its rewrites, and
written last, after relayout, which is what collects the words. It says how
many of the licensed words went, `0 of 0` when no rewrite freed one, names each
with the rule and address of the rewrite that freed it -- that is what you
switch off to keep it, since the reclaim has no ordinal of its own -- and, for
a word freed on `-O2`'s guess alone, says so and leaves it where it is. Under
`-O=undeclared` the guess licenses the word, and the line is marked
`(undeclared)`. A line of its own names any pool word relayout's rebuild added
or dropped. See "The pool reclaim: a constant nothing names".

**5. Classification conflicts.** Words that are two things at once -- code that
is also data, code whose address field is patched at run time, code that is
written. These are not suggestions. They are the places any future rewrite
would break, and they are worth a look on their own account.

**6. Apparently unreached code.** Blocks nothing was found to reach, with the
caveat that this is a hint and not a proof. See "What the advisor cannot know".

**7. Recursion.** Present only when the call graph has a cycle that is a property
of the program rather than of the analysis, so most reports do not have it at
all -- `adventure.am1` does not. Every call round such a cycle was resolved to
a single target, and the `dap rtn` return idiom cannot survive one: the second
call overwrites the first call's return address. The section names the
routines on each cycle and says what the two possibilities are. Cycles that
exist only because a call could not be followed, and cycles closed by a
routine that transfers out of itself instead of returning, are counted in
`-O=calls` and deliberately kept out of this section: reporting either one
would accuse a correct program.

**8. Return-word sharing.** Present in every report. This one is different from
everything above it and the section says so in its own first paragraph, at
length, every time.

Two routines can share one return word when neither can be on the call stack
while the other is. On `adventure.am1` that would free **116 words** — 46 in
bank 0, 23 in bank 1, 37 in bank 2 and 10 in bank 3 — which is more than the
whole peephole catalog recovers on the same program. Banks 1 and 2 are the two
that are short of space, and each gains about 45% more free space.

**It is advisory only, permanently, and by construction.** It is not a finding,
it is not counted in any finding total, and there is no flag that makes **am1**
act on it — not `-O` and not anything later. That is not caution about an
unfinished feature. Every other suggestion in the report fails visibly when it
is wrong: the program stops, or a test does. A wrongly shared return word fails
as a wild jump at run time, in a routine that works until the one call ordering
that overlaps, with no stack and no trap to catch it — and putting the old
source back will not reproduce it, because it depends on an interleaving and
not on the code. So this section names the routines and leaves the decision
where it belongs.

The section has three parts.

- **The leaf cut**, first and on its own. A routine whose body contains no call
  and no indirect jump the analysis could not follow cannot be running while
  another such routine is running, so every leaf in a bank can share one word.
  That is the whole proof, it fits in a sentence, and every routine listed under
  it can be checked against the source by eye. It is worth 43 words on
  `adventure.am1`, about a third of the total, for an argument you do not have
  to take on trust.
- **The full assignment**, per bank, one group per proposed word: the address
  proposed to keep, how many words the group holds, how many would be freed,
  and every routine that would share it, named. A group marked NOT FULLY KNOWN
  holds a routine whose body or exits the analysis could not read in full — 13
  of `adventure.am1`'s 140 return words, on the large routines — and those are
  the ones to look at hardest, because they are where the analysis could be
  wrong in the direction that costs a return address.
- **What it would cost to collect.** Two things the section is careful not to
  overpromise. A freed word is not a collected word: sharing turns N words into
  one and frees N-1, but those are scattered addresses inside routine bodies,
  and on a hand-laid source the space only becomes usable when the bank is
  repacked. And the edit is not local: a shared word cannot stay `local` to one
  routine, so every `dac rtn` and every `jmp i rtn` in every routine of the
  group has to name the one surviving address — which is why that address is
  named.

Two kinds of return word are excluded outright and counted where you can see
it. A `jda` routine's word is the one below its entry, at an address fixed by
where the routine sits, so there is nothing separable to share. And a word two
entries into one body name at different call depths is described by neither
depth, so it is given a group of its own and shares with nothing.

Use `-O=share` for the working: it prints every routine's color, pool, return
word and group, and reconciles the assignment against the call-depth figures
bank by bank.

**9. Scratch-word sharing.** Present in every report. It is the same kind of
advice as section 8 and about the same kind of storage, so it follows it, and
it carries the same warning in its own first paragraph: **it is advisory only,
permanently, and by construction.** It is not a finding, it is not counted in
any finding total, and there is no flag that makes **am1** act on it.

Where section 8 is about the words a routine keeps its return address in, this
one is about the storage words a program declares: the population is every word
carrying a `local` label, less the return words, the words of a text string,
and words that are code — **271 words on `adventure.am1`**. Two of them can
share one location when they are never live at the same time. On
`adventure.am1` that would free **109 words**: 18 in bank 0, 18 in bank 1, 70
in bank 2 and 3 in bank 3.

What is offered is only what is private to one **unit** — one routine, or a
group of routines control passes between without returning — and is never live
across a call the unit makes or on entry to it. A word whose address is taken,
one two units reference, one in a unit the analysis could not read in full, one
in code both the main line and a sequence-break handler run, and one that
carries a value between calls are all excluded. A word is shared only inside
its own bank and only inside its own pool, for the same two reasons section 8
gives.

The section has five parts.

- **The block-local class**, first and on its own: **31 words on
  `adventure.am1`**, from 36 that need 5. Every reference to a word in it lies
  in one block, the first of them a full write, with no `xct` between the first
  and the last, so the word is live between two addresses of straight-line code
  and a bank needs as many words as the widest overlap. That is the whole
  proof, and every word listed under it can be checked against the source by
  eye.
- **The pooled assignment**, per bank and per unit. Two words in different
  units are never live together, so a bank needs only as many words as its
  neediest unit; inside a unit the words are colored over their occupancy under
  pessimistic liveness, which makes the count an assignment that exists rather
  than an estimate. Each proposed word is printed with the address to keep and
  every unit that would share it, named.
- **What is not promised.** A word marked `near-taken` sits in a run of data
  words where some word's address is taken, and **am1** cannot see what pointer
  arithmetic from that address reaches. **32 of `adventure.am1`'s words are
  marked**, and the figure that stands without every one of them — **77** — is
  printed as its own number. Confirm a marked word against the source before
  counting it.
- **The call class, refused, with its number**: **77 words on `adventure.am1`**,
  the largest class of all. A word live across a call the unit makes may be
  used by the callee or by anything the callee calls. That is section 8's
  failure mode in data form, and ruling it out soundly needs the
  interprocedural liveness this analysis was chosen to avoid. The number is
  printed so that what is refused is known rather than merely missing.
- **What it would cost to collect**, with the same two warnings section 8
  gives: a freed word is not a collected word, and the edit is not local.

One blind spot is stated in the section itself: a device that writes memory on
its own — the drum, DCS2, the high-speed channel — writes words this analysis
never sees written, so a word one of them fills is not known to be live and
must not be shared.

Use `-O=scratch` for the working: it prints every unit, one line per population
word with the class it was put in and why, the colorings, the per-bank classes
and prizes, the blind spots, and a reconciliation line. The section and the
dump are two printers over one pass, and agree figure by figure.

**10. Statistics**, per bank: words by kind, the decode and spelling
breakdowns, the code/data classification, the reference edges by role, the
conflicts, the basic blocks, reachability, and a static cycle sum. A source
with no findings at all still gets a complete statistics block; that is the
half of the report that is useful when there is nothing to suggest.

**Inline expansion, under `-O1` or `-O2`, only when the source declares a
`%%speed` region or an `%%inline` marking.** Last in the report, because it is
written after relayout has made the copies and can say what became of each.
Its heading gives the level and the declared and copied counts. A paragraph
gives the cap, the reserve, the words spent, the words deleted with
single-site routines, the microseconds saved per execution of every copied
site, and how many copies relayout refused. After that come the words spent
per bank with its ceiling, `(declared)` when a `%%ceiling` set it, then one line per declared site: its bank,
address, source line and routine, and either `copied, N words, saves N us`
(with `callee deleted` when its only call was copied) or `not copied:` and
the reason in words. It is the same list as the `S1` lines of `-O=xform`.
The Transforms section, 4c, counts the T3s and T8s alone.
## The rules

Twelve rules. Every example below is taken from the optimizer's test sources,
where each rule has a source that fires it and one source per precondition
that refuses it.

| Id | Finds | Suggests | Saves |
|---|---|---|---|
| T1 | two consecutive operate words | one operate word | 1 word, 5 us |
| T1b | two consecutive shifts, same opcode | one shift, summed count | 1 word, 5 us |
| T2 | `skip; jmp .+2; W` | the inverted skip and `W` | 1 word, 5 us |
| T3 | a `jmp` to a word that is itself a `jmp` | jump straight to the far target | 5 us per hop |
| T6 | `cla` before an instruction that fills AC | drop the `cla` | 1 word, 5 us |
| T7 | `cli` before a `lio` | drop the `cli` | 1 word, 5 us |
| T8a | `lio` of a pool word holding 0 | `cli` | 5 us, 1 pool word |
| T8b | `lac` of a pool word holding 0 | `cla` | 5 us, 1 pool word |
| T8c | `lac` of a pool word holding 777777 | `cla cma` | 5 us, 1 pool word |
| T8d | `lac` of a pool word that fits in 12 bits | `law` | 5 us, 1 pool word |
| T13 | `jmp .+1` | delete it | 1 word, 5 us |
| T14 | an AC/IO copy through a temporary | a PDP-1D `lia`, `lai` or `swp` | 1 to 3 words, 15 to 35 us |

### T1, two operate words merge

```
        cla                     0200 760200
        cma                     0201 761000
becomes: cla cma                (one word, 761200)
```

The operate group is a bag of micro-op bits, so two operate words can often be
written as one word carrying both. Often, not always. The rule uses the
hardware's own phase order -- `cla`/`cli` at TP7, the transfers and the flag
field at TP8, `cma`/`hlt` at TP9, and the PDP-1D `lia`/`lai` completing in the
*next* instruction's fetch -- and refuses the merge when the two words in
sequence would not do what one merged word does:

- `phase`: the hardware would apply the merged micro-ops in the other order.
  `cma` then `cla` as two words leaves AC zero; merged into one word the `cla`
  acts first and the `cma` leaves all ones.
- `flags`: both words touch a program flag and there is only one flag field.
- `repeat`: a micro-op that is not idempotent is in both words. Two `hlt`s are
  two stops; two `cma`s cancel.
- `lap`: the second word carries `lap`, whose value is its own address plus
  one, so moving it changes what it loads.
- `exchange`: `lia` and `lai` in separate words are two copies. Merged into one
  word they are a single exchange, which is a different thing.

### T1b, two shifts merge

```
        ral 3s                  0200 661007
        ral 2s                  0201 661003
becomes: ral 5s                 (one word, 661037)
```

Refused with `count` when the summed count is more than the nine a shift word
can hold.

### T2, skip over a jump

```
        sza                     0200 640100
        jmp .+2                 0201 600203
        dzm tmp                 0202 340300
becomes: sza i
```

The `jmp` exists only to skip `W`. Inverting the skip does the same job in one
word fewer. `W` itself does not move in the source. Refused with `noinverse`
for a skip-class word that has no inverted form.

### T3, jump to a jump

```
        jmp t3hop               0200 600210      and 0210 holds jmp t3end
becomes: jmp t3end
```

Layout-neutral: no word is added or removed, so nothing moves. The word at
0210 is left alone and any other route through it still works. The saving is
`5 us per hop taken` rather than a flat figure, because it is paid only when
that jump is executed.

Refused with `patched` when the intermediate word's address field is written
at run time -- following it at assembly time would freeze a target that the
program means to change. In real code this is the commonest refusal there is:
a subroutine return word that a `dap` writes is exactly this shape.

Refused with `through-taken` when the intermediate word's address is used as a
value anywhere: by a `law`, or by a data word holding it. A pointer loaded from
that value can write the word, and the analysis cannot follow the pointer. A
table of patches applied at run time is exactly this shape, and the refusal is
blunt on purpose. A table of addresses the program patches and a table of
values it writes look the same to the analysis, so a word named in either is
refused.

`-O1` rewrites a live T3 inside a region, and follows a chain of jumps to its
far end: the rewritten jump names the last target, not the first hop's. See
"`-O1`: rewriting inside a region".

### T6 and T7, a clear before a load that clears anyway

```
        cla                     0200 760200            cli            0200 764000
        lac tmp                 0201 200300            lio tmp        0201 220300
becomes: lac tmp                                becomes: lio tmp
```

`lac` fills all of AC and `lio` all of IO, whatever was there before, so the
clear in front of them changes nothing.

T6 also covers `law`, and `lat` and `lap` -- but only when those words carry
the `cla` bit themselves, which is how `permsyms.def` spells them (762200 and
760300; the handbook notes they are usually combined with a clear). `lat` and
`lap` OR into AC rather than filling it, so without that bit the leading `cla`
is not redundant and the pattern is not a match at all.

A `cla` before a `lap` is claimed by T6 and then **refused**, with reason
`lap`: dropping the `cla` moves the `lap` one word lower, and `lap` ORs in its
own address plus one, so the value it loads would change. It is claimed and
refused rather than never looked for, which is what puts it in the suppressed
section where you can see it.

T6 and T1 would both fire on a `cla` in front of an operate word that clears
AC itself, and would suggest the same single word. T6 takes the pair, because
"the `cla` is redundant" is the more useful thing to be told.

### T8a to T8d, a constant that does not need a pool word

```
        lio [0]                 becomes: cli
        lac [0]                 becomes: cla
        lac [777777]            becomes: cla cma
        lac [0o144]             becomes: law 0o144
        lac [~0o144]            becomes: law i 0o144
```

Layout-neutral -- one word becomes one word -- and the only family that needs
no analysis beyond decoding. The saving is in time and in the literal pool: if
nothing else in the program names that pool word, the pool loses a word too,
which the note says explicitly.

T8d applies to values from 0 to 07777, and to their complements through
`law i`. Where the source named the value with an address symbol, the
suggestion keeps it: `lac [buf]` becomes `law buf`, not the address `buf`
happens to have in this build.

Refused with `through-taken` when the pool word's own address is used as a
value, for T3's reason.

Note that the pool is shared by *value*. Two sources that both write `[0]`
name one pool word, so the pool only shrinks when every reference to that word
is rewritten.

`-O1` rewrites all four inside a region. It does not shrink the pool: a pool
word no rewritten word names any more is counted as freed and still emitted,
so that no pool word after it moves.

### T13, a jump to the next word

```
        jmp .+1                 0200 600201
becomes: (delete the word)
```

### T14, AC/IO copy through a temporary

```
        dac t                   becomes: lia
        lio t

        dio t                   becomes: lai
        lac t

        dac ty                  becomes: swp
        dio tz
        lac tz
        lio ty
```

`lia` copies AC into IO, `lai` copies IO into AC, and `swp` exchanges them.
Each replaces a two-word copy through a scratch word, and the scratch word
becomes free as well: the exchange form saves three words and two temporaries.

The rule refuses with `tempused` when the temporary is reached from anywhere
outside the pattern -- if something else reads it, the rewrite would stop
writing a word that something else depends on.

**These are PDP-1D instructions.** Per the owner's decision (study section 9)
T14 fires on every source, not only on sources that already use PDP-1D
mnemonics. Taking the suggestion ties the program to a PDP-1D, which every
current PDP-1 emulator implements but no real PDP-1 had. If that matters for
your program, T14 is the one rule to read past.

## The preconditions

Four are checked. The fifth cannot be, by anyone but you.

**P1** -- no label, jump target or program entry inside the pattern, so
control cannot arrive anywhere but at its first word. A rewrite that merges or
deletes an interior word assumes nothing jumps to it. Reported as `P1-label`
when a label sits on an interior word and `P1-entry` when something jumps into
one.

**P2** -- no word of the pattern is written or patched while the program runs,
and no word's address is used as a value. Reported as `P2-written` (a `dap`,
`dip` or `dac` writes it) and `P2-taken` (a `law` or a data word carries its
address).

**P3** -- no word of the pattern is executed in place by an `xct`. Reported as
`P3-xct`.

**P4** -- no skip-class word immediately before the pattern. A skip changes
which of the pattern's words run, so a rewrite that assumes both run is wrong.
Reported as `P4-skip`.

Two more refusals apply to every rule. They ask whether the pattern's words are
instructions at all, and they are tested only on a finding nothing else
refused:

- `unreached` -- a word of the pattern is in code no entry reaches, and no word
  of that unreached run carries a label. A data table whose values are spelled
  as instructions (`cla`, `sal 1s`) looks exactly like unreached code. The
  label is what spares real code the analysis loses track of, for example
  after an IOT spelled as a number: such code is almost always named.
- `table` -- the pattern is in a run of words that nothing jumps, falls, skips
  or calls into, that is reached only because its first word's address is
  used as a value, and that runs into data. That is a table used through its
  address, such as a list of shift instructions run by an `xct` the program
  builds at run time, which P3 cannot see.

**P5** -- no location involved is shared with a device, a sequence break
handler, the drum, DCS2 or a high speed channel.

**P5 is not derived, ever.** The assembler cannot know which words another
agent reads or writes behind the program's back. A load that looks redundant
may be polling a location a device updates. A word that looks dead may be a
mailbox. And timing is the same kind of thing in reverse: a delay loop for the
typewriter, the punch or the display is meant to take the time it takes, so
making it faster breaks it.

Only the author can settle P5. What the advisor now offers is a place to write
the answer down: the `%%optimize`/`%%endoptimize` directive, above. A source that
declares no region is told in the header, in the same words it always used,
that P5 is not checked and must be reviewed by hand; a source that declares one
has said what P5 asks, and the report checks two thirds of that declaration
against the reference analysis and names every word where the two disagree.

That does not make P5 derivable and it does not withhold any advice. It moves
the sentence from a caveat nobody can act on to a statement the author has made
and the tool can argue with.

`-O2` does not derive P5 either. It guesses, by six heuristics, and says so
every time it runs. A `%%nooptimize` span is the author's other answer: not
"this code is safe to rewrite", but "leave it alone".

## What the advisor cannot know

Two of the report's sections are heuristics, and both say so where they print.

**Unreached code is a hint, not a proof.** A block appears there when nothing
was found that reaches it. Several ordinary constructions produce false
entries:

- A dispatch table of `jmp` words is read as data and never followed as a
  jump, so a routine entered only through one looks unreached.
- Anything reached only through a pointer that could not be followed, or
  through an address field a `dap` writes, looks unreached.
- **The inline argument, which used to be the commonest cause by far.** A call
  whose argument word follows the call, and a callee that returns past that
  word, once lost its return edge: the return was assumed to go to the word
  after the call, that word is the argument, the argument is data, and so
  everything the caller would have reached afterwards was called unreached.
  Where a call returns to is now asked of the callee instead, by walking every
  path through it, and the answer is one of three: the callee returns to the
  word after the call; it steps its saved return address past N argument words
  and returns to `call+1+N`; or it could not be read. The first two are
  proved. The third keeps the assumed edge -- dropping it would make the
  caller's tail look unreached, which is the worse error -- but it is counted
  and named rather than presented as fact. The report gives the three counts,
  and `-O=flow` names every call site, the address it returns to, and for an
  unproved one the reason. A caller of a callee that could not be read can
  therefore still appear in this list; what has changed is that you can look
  up which callee and why instead of guessing.

The statistics' `flow:` line says how much of this there is. Its `to the
unknown sink:` counts are every edge the graph could not follow, and `of which
routine returns:` counts the ones that are a routine leaving through its own
return word: `rtn, jmp .` whose address field the entry's `dap` wrote, or
`jmp i rtn`. Those lose nothing, since every call site keeps its own edge back,
so the threads really lost are the rest. Two kinds of edge are followed that
once were not, and `recovered:` counts them: `iot`, the skip of a Type 340 flag
test (`dsp`, `dss`, `dsv`, `dsh`), and `stored`, a target of a `jmp` or `jsp`
that a `dac` or `dio` stores whole, where the code in front of every store says
what it stores. Where one store does not say, the sink stays beside the stored
targets. `-O=flow` marks each recovered edge with `iot` or `stored` and each
returning sink with `return`.

And one caveat the other way: an `xct` is not treated as a block terminator,
so the word after an `xct` of a jump is called reached when it may never run.

**Code and data classification is derived, not declared.** A word is called
code because something jumps to it or falls into it, and data because
something reads or writes it. A word that is genuinely both shows up in the
conflicts section, which is the right answer, but a word reached only by a
route the analysis could not follow may be classified on incomplete evidence.

The rules do not fire on evidence like that -- P1 and P2 are exactly the
checks that stop them -- but the statistics and the unreached list are
reporting what was found, not what is true.

**Sequence-break frames are assumed from two pieces of evidence.** A frame is
the four words of bank 0 low core a channel owns: the hardware stores AC, the
return PC and IO in the first three and begins executing at the fourth, so
word 3 of channel 0's frame, 7 of channel 1's and so on are entries that
nothing in the program names. The advisor decides which of them exist this
way:

- **Is there an enable command anywhere in the program?** The enable commands
  are `esm`, `asc` and `isb`. `lsm`, `dsc` and `cbs` are not. The advisor tests
  every emitted word, code or data, by its device code, so an `asc` carrying a
  channel number counts, as does an enable written as a number or built by a
  macro, or a data word that happens to equal one. With no enable command
  there are no frames, and bank 0's 0 to 077 is ordinary code and data.
- **If there is one, what is below the start address?** The frames are the ones
  lying wholly below it: none for a start below 4, channel 0's alone for a
  start of 4 (the single-channel system's convention), all sixteen for 0100 and
  above (the sixteen channel convention). A program that ends with `stop`, or
  starts in another bank, keeps all sixteen.

The report's header says which frames were assumed and names the first enable
command it found. `-O=flow` and `-O=calls` print every one of them on their
`sbs enables:` line. A program that enables breaks but starts below 4 gets a
warning, since its own code covers channel 0's frame.

Neither test can see a system that is already on when the program starts, left
that way by a loader or by the program run before it, or an enable command the
program builds at run time or loads from elsewhere. Guarding against those is
the author's part. A program that must not be interrupted conventionally puts
`lsm` among its first instructions, and then the assumption of no frames holds.

## Instruction codes and the PDP-1D

The decoder reads every emitted word, whatever the source called it. Codes 00,
12, 14 and 36 are spare on every PDP-1 and decode as unknown; a word carrying
one is still counted and still classified, it simply has no mnemonic. Code 12
held `jfd` in earlier versions of `permsyms.def` and no longer does.

Code 74, the PDP-1D special operate group, is always decoded as such, on every
source. It is spare on a plain PDP-1, so a word with that code is a halt
there; the advisor reads it as the 1D group because per the owner's decision
the 1D decode is always on.

## The extend window: `eem` and `lem`

On a PDP-1D, `eem` opens the extend window and `lem` closes it. With it closed,
an indirect reference reads a 12-bit address in the current bank, and bit
010000 of the pointer chains to another level; with it open, the pointer is a
16-bit address, one level. So the same `lac i p` reads a different word
depending on the state, unless the pointer holds a bank-0 address below 010000
and the reference is made from bank 0, where both readings agree. Closed, the
bits above 010000 are ignored, so a 16-bit address reaches its word only when it
names the current bank and that bank is even. A `jsp` leaves the whole 16-bit
return address in AC, bank bits included, whatever the state, and with the
window open it sets 0200000 as well. (The closed-window facts were measured on
the emulator, 21-Sep-2026.)

Under `-O` the advisor follows the state -- open, closed or not known --
through every reached word and across calls, and follows backward what
depends on it: an indirect reference, an `xct`, a `lap`, another device-74
`iot`, or a call into a routine that may depend on it. It then classes every
reached `eem` and `lem`:

- **redundant**: it sets the state the window is already in on every path.
- **dead** (`lem` only): nothing that depends on the window runs after it
  before the window is set again, so the state it sets is never used.
- **freed** (`eem` only): not redundant as written, but redundant once every
  redundant and dead word is taken out.
- **needed**: something depends on the state it sets.
- **refused**, with one reason: the analysis cannot say. The reasons, in the
  order the report counts them, are `patched` (the word is written or patched),
  `xct` (it is executed by `xct`), `sbs` (the program enables sequence breaks,
  and a handler could change the window between any two words), `timing` (it is
  in a delay or device loop), `skip` (a skip can pass it), `start` (the state
  at the start address is not recorded), `entry` (it can be entered from
  outside the flow the analysis sees), `halt` (Continue resumes in an unknown
  state), `callee`, `unknown-return`, `lost` (control leaves the graph),
  `shape`, `pointer` (an indirect reference through a word whose value is not
  known follows), `flag` (something reads the flag a `jsp` leaves in AC) and
  `iot`.

Outside a declaration nothing acts on these classes: they are there so that
you can take out an `eem` or `lem` the program does not need, by hand, and test
the result. Inside one, `-O1` and `-O2` take them out for you.

### Deleting `eem` and `lem`

Under `-O1` or `-O2`, every redundant `eem` or `lem`, dead `lem` and freed `eem`
in an **optimize or speed region** is deleted, one relayout delete each, and
every later word moves down as it does for space mode's deletions (see "Space
mode: deleting a word"). A word outside every region is left alone at every
level, unless `-O2` is given `-O=undeclared`, which lets the guess license it.

Any set of redundant and dead words can go together: taking one out never
changes what the window is at a word that depends on it. A freed `eem` is
different. It is redundant only because a `lem` before it goes, so the analysis
finds, for each freed `eem`, the `lem`s it depends on -- by putting each one
back alone and seeing whether the `eem` still finds the window open -- and
deletes the `eem` only when every one of them is deleted too. Words are refused
in this order, with a fate the dump and the report name:

- `handsoff`: the word is in a `%%nooptimize` span.
- `guessed` (`-O2` only): the guess marks it -- H1 in an optimize region, any
  of the five on a word only a speed region declares or, under
  `-O=undeclared`, no region -- as for space mode.
- `used`: the program reads the word as data, to copy or sum its code.
- `unrepresentable`: relayout could not delete the statement.
- `overlap`: another rewrite touches it -- a pattern, a T3 or T8 word, an inline
  copy, a move or an unroll -- so bisection can switch either off on its own.
- `depends`: a freed `eem` with a `lem` it depends on that is not deleted.

A delay or device loop never gets this far: the analysis refuses an `eem` or
`lem` there as `timing`, at every level.

`-O=window` gives one line per licensed word:

```
window delete 0 0106 freed eem fired deps 0104,0105 line 21
```

with the kind, the fate, the ordinal under a bisection modifier, the `lem`s a
freed `eem` depends on, and the line. The report's Extend window deletions
section lists every licensed word with what happened to it, and marks one only
`-O=undeclared` licensed `(undeclared)`. Bisection numbers
the deletions after space mode's, redundant and dead words first, then freed
`eem`s, so an `-O=upto` that keeps a freed `eem` keeps its `lem`s; a range or an
`-O=off` that switches off a `lem` switches off the `eem`s that depend on it
too. If relayout refuses a `lem`'s delete -- because a number, an offset or a
`.` would name a different word -- an `eem` that depends on it is not tried
either, and a warning says so for each:

```
am1: warning: window: relayout refused the dead lem's delete at bank 0 0103 (number), so it stays; prog.am1:13
am1: warning: window: the freed eem at bank 0 0105 stays, because a lem it depends on stays; prog.am1:15
```

With `-O=relayout`, a `window:` line per deletion says whether relayout made it.

On `adventure.am1`, with the regions `genregions.py` declares, `-O1` and `-O2`
each delete 117 words -- 9 redundant `eem`s, 9 redundant `lem`s, 72 dead `lem`s
and 27 freed `eem`s -- and leave 5 that overlap another rewrite. Relayout makes
all of them, and the assembly takes about two seconds longer.

A **hazard** is an indirect reference in a bank other than 0 made with the
window possibly open. It is **proved** when the pointer is never written and
holds a bare in-bank address -- a label or a symbolic constant below 010000,
with no bank qualifier -- and the window is certainly open: the reference
reaches bank 0, not the word the address names in its own bank. It is
**possible** when the same pointer is used with the state not known. It is
**unproved** when the pointer is written at run time and the window is open;
most of these are 16-bit pointers written on purpose, and the analysis cannot
see their values. A return word saved by the routine's own entry is never a
hazard: the `jsp` contract makes it a 16-bit address. A plain number is never
taken as a bare address, so `farjmp(0)` is not flagged.

Each proved hazard is also a warning on stderr, one line, so a build log shows
it without the report:

```
am1: warning: extend window: prog.am1 line 33: bank 1 0104 indirects through the bare address at bank 1 0112 with the window possibly open, which reaches bank 0
```

A program that copies code into bank 0 at run time and indirects through its
bank-0 address on purpose draws this warning; that is the analysis reading the
source as written. The assembly still succeeds.

A **reverse hazard** is the other way round: an indirect reference made with
the window possibly closed through a pointer meant as a 16-bit address, which
the closed window misreads. A pointer is taken as meant when a bank qualifier
(`w:1`, `w:*`) is among its leaves or its value is above 07777; a plain number
no larger is taken as twelve bits. It is **proved** when the pointer is never
written, the window may be closed, and the closed reading misses the word --
it names another bank, or its own bank is odd and the reference chains. It is
**possible** when the same pointer is used with the state not known, and
**unproved** when the pointer is written at run time from such a constant
(`lac [w:1]` then `dac p`) and the window may be closed.

A routine's return word is a pointer too. A `jsp` saves its caller's 16-bit
address there, so a `jmp i rtn` (or `lac i rtn`) made closed misreads it
whenever the caller's bank is odd or is not the routine's bank: in bank 1 a
`dac rtn` ... `jmp i rtn` routine called and left with the window closed does
not return. That is **proved** when such a caller may call with the window
closed. When the only misreading callers call with it open -- a call from
another bank must -- and the window is closed at the return only on other
callers' paths, it is **possible** with the reason `callers`. A `dap`-form
return keeps 12 bits and is never a reverse hazard.

Each proved reverse hazard is a warning under the same prefix:

```
am1: warning: extend window: prog.am1 line 14: bank 0 0101 indirects through the 16-bit address at bank 0 0114 with the window possibly closed, which reads bank 0 instead of bank 2
am1: warning: extend window: prog.am1 line 46: bank 1 0116 indirects through the return word at bank 1 0117 with the window possibly closed, which misreads the address a call from bank 1 saves there
```

A pointer into an odd bank says "chains through bank N" in place of "reads".

An `eem` or `lem` no entry reaches is classed **unreached**, and the state
after it is not followed, so no hazard or reverse hazard is decided for what
it guards. Those references are not passed over in silence: each indirect
reference its flow arrives at, before that flow joins code the analysis did
reach, is an **unexamined** reference. The dump gives each one a
`window unexamined` line with its pointer, the totals count them
(`window totals: unexamined references N`), and the report lists them. The
usual way into such code is a jump whose address is written at run time from
a value the analysis cannot know, `lat` then `dac` for instance; a
defect there -- a proved reverse hazard, say -- shows up only as an
unexamined reference, so a dump that has any is not a clean bill of health
for them. References before the `eem` or `lem` in its own block are not
counted: it does not guard them.

On `adventure.am1`, of 211 reached `eem`s 9 are redundant and 29 more freed, and of 202
reached `lem`s 10 are redundant and 75 dead; it has no proved hazard and 117 unproved,
and the analysis takes about a tenth of a second. It has no proved or unproved reverse
hazard; 1010 references are possible ones, almost all through bank-qualified pointers in
routines the analysis cannot see entered (`entry`, 681) or returned to (`unknown-return`,
311), and 18 are return words closed only on other callers' paths (`callers`).

## Testing

**am1** ships with the checks that hold its output, all under `Tools/AM1`.
Each takes the binary to test as its first argument, and exits non-zero if
anything failed.

| Check | What it holds | Binary it tests by default |
|---|---|---|
| `Tests/Regression/run_regression.sh` | the assembler's regression suite: each source must assemble, or for an expected failure must not, and the words it generates must equal a stored reference | the `am1` of the tree it sits in |
| `Tests/Multibank/run_tests.sh` | assembling across memory banks | the `am1` of the tree it sits in |
| `Tests/Symbols/symbols_check.sh` | the line a global label, or a var, is recorded on in the `.sym` file, whatever follows the label | `am1test` |
| `wordtable_check.sh` | the optimizer's word table, `-O=dump`, against the `-T` dump and against the words decoded from the tape, for every regression source | `am1test` |

Build **am1test** first for the last two: `make am1test` in `Tools/AM1` (see
"The testing build"). For example, from `Tools/AM1`:

```bash
make am1 am1test
Tests/Regression/run_regression.sh ./am1
Tests/Multibank/run_tests.sh ./am1
Tests/Symbols/symbols_check.sh ./am1test
./wordtable_check.sh ./am1test
```

With `-O` absent, **am1** must assemble every program exactly as it would with
no optimizer in it. The regression suite holds that: every source's words are
checked against a stored reference, and none of them uses `-O`.
`wordtable_check.sh` holds the other end: the word table every analysis reads
must be the program the tape carries, word for word. Given a second binary
built without the optimizer as its third argument, it also requires the two
builds' `.rim`, `.lst`, `.sym` and `.dmp` to be byte-identical for every
regression source.

The optimizer's own acceptance checks, one suite per analysis and per level,
each with a leg built to fail, are development material and do not ship with
**am1**. They are where the figures in this document come from.
