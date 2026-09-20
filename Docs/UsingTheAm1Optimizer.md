# Using the **am1** optimizer advisor, `-O`, `-O1` and `-O2`

This document describes the `-O` flag and the report it writes, and the `-O1`
and `-O2` flags, which act on the code.

This is version 1.20 and covers up through am1 version 1.50; it will be updated as needed.\
Edit date 19-Sep-2026\
Directive naming changed to use %%, e.g. %%speed.

## What the advisor is

`-O` makes **am1** read the program it has just assembled and write a report
naming places where the source could be shorter or faster.
It is an advisor, no code is changed.

`-O1`modifies code  and it is narrow in scope on purpose.
It rewrites a word only where you have declared a region,
only where a finding has passed every check the advisor can make,
and only for the five rules that replace one word with another at the same address.
Nothing moves.
See "`-O1`: rewriting inside a region" below.
Without a region anywhere in the source, `-O1` does nothing.

`-O2` makes the same five rewrites without asking for a region.
It guesses instead, and the guess can be wrong, every `-O2` assembly says so on stderr.
See "`-O2`: rewriting on a guess" below.

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
`-O1` takes only the part of that decision that neither reason touches.
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
| `-O=xform` | prints on stdout what `-O1` or `-O2` does with every live finding: rewritten, or which reason it was not; without either a dry run of `-O1` that rewrites nothing. With a speed declaration, also one `S1` line per declared call site: copied, or which reason it was not (see "Inline expansion"). With any region, also one `S4` line per declared placement site: moved, or which reason it was not (see "Fall-through placement"), and one `S2` line per declared loop: unrolled, or which reason it was not (see "Loop unrolling") |
| `-O=upto=N`, `-O=range=B:LO-HI`, `-O=off=FILE` | with `-O1` or `-O2`, switch rewrites off, to find the one that broke a build. See "Finding the rewrite that broke a build" |
| `-O=inline=cap:N`, `-O=inline=reserve:N` | set inline expansion's body cap (8 words by default) and each bank's reserve (64 words by default), decimal, 0 to 4096. See "Inline expansion" |
| `-O=place=undeclared` | under `-O2`, let fall-through placement move a site no region declares, on the guess. Refused without `-O2`. See "Fall-through placement" |
| `-O=unroll=cap:N` | set loop unrolling's cap, the most words a loop may become (64 by default), decimal, 0 to 4096. See "Loop unrolling" |

Any other modifier is refused with the usage text.

### The testing build: `am1test`

The optimizer has fifteen more modifiers, each of which exists to test it:
the debug dumps of each analysis, the decoder's self-check, and relayout's
edit file. Since 19-Sep-2026 (task A34) the installed **am1** does not
recognize them, so they do not clutter its usage text. **am1test** does. It
is **am1** built with `AM1_TEST_SWITCHES` defined, by `make am1test` in
`Tools/AM1`, and it is never installed. The checks under `Tools/AM1/Tests`
run it. Apart from these switches, it assembles every program byte for byte as
**am1** does, and the `switches` check proves that. The one other difference is
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
| `-O=regions` | the `%%optimize`/`%%endoptimize` regions, their extents, and every word whose region's declaration the evidence contradicts; then the `%%nooptimize` spans, when the source declares one; then the `%%speed` regions and the `%%inline` markings, when it declares one of those (task A32 step 2). `-O1` and `-O2` act on the speed declarations: see "Inline expansion" |
| `-O=rules` | the findings, one per line, live and suppressed |
| `-O=scratch` | the scratch-pool measurement: every storage local, the class its rules put it in, and what sharing the pool could free, block-local and pooled, per bank. The report's scratch-word sharing section is written from it (tasks A16 and A24) |
| `-O=guess` | what `-O2`'s heuristics mark: the loops and their cycles, each classed plain, delay or device; the handler territory; device buffers; addresses written as numbers; and every live finding with the heuristics that refuse it. A measurement: it refuses nothing and changes no word |
| `-O=values` | what each basic block already knows about AC and IO: every load whose register already holds the value, or that one word could replace (`lai`, `lia`, `law`), and every load or clear whose result the block overwrites unread; with the safety flags on each, the per-bank counts and what the pass cannot see. A measurement only (task A21): it changes nothing, and no finding comes of it |
| `-O=speed` | what speed mode could do, site by site: inline expansion, loop unrolling, jump chains followed to their end and fall-through placement, each site eligible or refused with one reason, what an eligible site saves per execution and spends in words, with tallies per bank and each bank's free words. A site whose callee could not be copied at all -- a label defined in the copy, or a copied word naming a word of the body -- is refused `notcopyable`, and the line names the word that broke it. A placement whose `jmp` the program uses as a word is refused `jmpused`, and one whose code reaches a `hlt` is refused `falls` (task A32 step 5a). A loop whose body has a label but its own or names a word of the loop is refused `internal`; one with a word something may write, or whose setup, `isp` or `jmp` the program uses as data, `used`; one whose body reads AC before loading it, `acentry`; and one whose next code reads AC before loading it, `acexit` (task A32 step 5b). A measurement only (tasks A27 and A32 step 1): it changes nothing, and adds nothing to the report |
| `-O=relayout` | lays the program out again, after any `-O=edits` edits, and prints what happened: each edit accepted or refused with its reason, where every moved word went, where every copy went, and the pool slots gained and lost. A testing instrument (task A22, extended by A32): with no edit it changes nothing. See "Testing relayout" |
| `-O=check` | runs the decoder's built-in self-check first, then proceeds |
| `-O=edits=FILE` | not a dump: make the edits FILE lists, then lay the program out again. Relayout's test instrument; see "Testing relayout" |

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
an edited word would then compute the wrong one. No rewrite changes length yet
(see "Testing relayout"). When one does, relayout checks the static cases: an
edit is refused if a number, an offset or a `.` would name a different word. A
computed address it cannot see. Timing, the drum and computed addresses are
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
guess. Loop unrolling has one of those two: an optimize region licenses it
too, and the guess never does.

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
expansion, fall-through placement and loop unrolling**, below. A marking that names no routine is reported and not
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
   any mark on the site or the routine refuses it (`guessed`). The guess can
   only refuse. The declaration is what licenses the copy.
3. **The edit must be writable** (`unrepresentable` if not). A `jda` must name
   its word by one plain name, since the `dac` is written with it, and a
   retargeted return must not hold a constant.
4. **A routine with one call, and a marked routine**, are copied whatever
   their size, as long as the bank has room below its ceiling (07750 in bank 0,
   07777 elsewhere). A copy that does not fit is `full`.
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

**`-O=place=undeclared`** licenses it everywhere else as well, on `-O2`'s
guess, including the run of a declared `jmp`. It is refused without `-O2`, and
with it stderr gains two warning lines. Use it when you would rather test the
program than declare it: the guess sees what the program does with its code
as written, and **an address the program computes at run time is not seen**.
A table of jump targets built by arithmetic, or a `jmp` found by counting
from a label, is exactly what it cannot find. A line moved under the switch
says `undeclared` where a declared one says `region`.

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
   the run refuses it (`guessed`). As for every rewrite, the guess only
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
undeclared loops.

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
   (`-O=inline=reserve:N`); one that does not is `budget`.

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

## `-O1`: rewriting inside a region

```bash
am1 -b -d -O1 program.am1
```

`-O1` is `-O` plus one step: after the analysis, and before any output is
written, it rewrites some words. `-O2`, below, is the same step with no region
needed. There is no other level: `-O3` and above are refused, and so are
`-O2s` and `-O2t`, the space and speed modes, which are not built yet.

### What it rewrites

Five rules, each of which replaces one word with one word at the same address:

| Rule | The word | Becomes |
|---|---|---|
| T3 | `jmp A`, where A holds `jmp B` | `jmp B` |
| T8a | `lio [0]` | `cli` |
| T8b | `lac [0]` | `cla` |
| T8c | `lac [777777]` | `cla cma` |
| T8d | `lac [n]`, n from 0 to 7777 | `law n` |
| T8d | `lac [~n]` | `law i n` |

Nothing is inserted, nothing is deleted, no label moves and no symbol changes,
so every other word of the program is where it would have been without the
flag. The other rules -- T1, T1b, T2, T6, T7, T13 and T14 -- all change the
program's length, and stay advice. So does return-word sharing, permanently.

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
4. The rule is **one of the five.** Any other live finding is put down as *a
   rule -O1 does not carry, because it changes the program's length*.
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

A pool word that no instruction names any more is **freed but not
reclaimed**. It is still emitted, at the same address, holding the same value,
because removing it would move every pool word after it. Reclaiming it is
relayout's job, and relayout does not exist yet. The report counts these
words.

### The checks it makes on itself

Each rewrite is checked three ways as it is made:

- the rule must not change the program's length;
- the new expression must assemble to exactly the encoding the rule names;
- that word, decoded afresh, must put the pool word's value, or the far
  target, where the original word did, with the other register untouched.

A failure of any of them is an internal error in **am1**, and it stops the
assembly before a single output file is written, the `.opt` included. It looks
like this, from a deliberately broken build recorded in
`Tools/AM1/Tests/Transforms/README.md`:

```
am1: internal error in -O1: T8b at bank 0 0102 built a word that assembles to 760200, not the 764000 the rule names
at line 21, file t8b_test.am1
```

None of them is expected to fire. If one ever does, the program is not
assembled at all, and the message is the thing to report.

### What it reports

The header of an `-O1` report says it is also the record of what `-O1`
changed, and a Transforms section follows the Optimize regions section. From
`Tools/AM1/Tests/Transforms/fates_test.am1`, after the section's opening
paragraph:

```
  Rewritten: 1
    T8d  bank  0 0101  fates_test.am1:36  region 1
        before 200114  lac [0o7]
        after  700007  law 0o7

  Not rewritten: 3
    T8d  bank  0 0100  fates_test.am1:34  outside every declared region
    T1   bank  0 0103  fates_test.am1:38  a rule -O1 does not carry, because it changes the program's length
    T1   bank  0 0106  fates_test.am1:41  straddling a region boundary
```

After the lists it says what the rewrites realized: storage words freed and
not reclaimed; microseconds from one pass through the rewritten words, 5 per
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
Without `-O1` it is a dry run: it says what `-O1` would do and changes nothing,
not even the report. A rewritten T3 that followed more than one hop ends
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
contradiction check reports clean, `-O1` rewrites 134 words: 126 T8d, 6 T3 and
2 T8a. Task A18 ran Adventure's 78-test suite against both builds, and they
reached the same pass/fail set.

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

Five heuristics make it. Each marks words, and a finding is left alone when a
heuristic marks any word of its **footprint**: the words of its pattern, plus
the pool word a T8 loads or the intermediate jump a T3 reads through.

| Id | Marks | The rewrite assumes |
|---|---|---|
| H1 | handler territory: every block a sequence-break entry reaches, calls included, and every word such a block writes | no sequence-break handler reaches or writes it |
| H2 | a delay loop: a cycle that writes nothing but its own counters, with no call and no in-out transfer | not in a delay loop |
| H3 | a device loop: a cycle that holds an in-out transfer to a device, or executes with `xct` a word the program writes | not in a loop that transfers to a device |
| H4 | a device buffer: a word whose address is loaded where an in-out transfer runs | not a device buffer |
| H5 | an absolute address: a memory reference written as a number, and the word it names | no address involved is written as a number |

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
memory with a device, but it does not say no handler reaches it. A finding only
partly inside a region gets all five, as one outside every region does.

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
heuristics it assumed. From `Tools/AM1/Tests/Heuristic/heur_region_test.am1`:

```
  Rewritten: 2
  Each rewrite rests on the guess that no heuristic it names found anything:
    H1  no sequence-break handler reaches or writes it
    H2  not in a delay loop
    H3  not in a loop that transfers to a device
    H4  not a device buffer
    H5  no address involved is written as a number
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
`assumes H1,H2,H3,H4,H5` or whichever applied, a left-alone line `by` the
heuristics that marked it, and a line in a span `handsoff`.

### What stays yours

`-O2` is for programs you will test. On `adventure.am1`, with no region, it
rewrites 132 words (124 T8d, 6 T3 and 2 T8a) and puts 15 findings down to its
guess: 13 in device loops and 2 in delay loops, among them the delay routine at
bank 2 7034. The guess is applied before the rule is asked about, so some of
the 15 are rules no level carries yet. Every word it rewrites is one `-O1` could rewrite inside a region, so
the checks it makes on itself are `-O1`'s, and a second pass over the
rewritten program rewrites nothing. Task A25 ran Adventure's 78-test suite
against the `-O2` build and the stock one, and they reached the same pass/fail
set. That is one program. Run yours, and when it breaks, the next section
finds the rewrite that did it.

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

**One rewrite uses relayout: inline expansion**, and only inside a speed
region or on a routine the author marked (see "Inline expansion"). Its copies
and deletes are edits like the ones below, made after any `-O=edits` edits,
and `-O=relayout` prints them as `inline:` lines, `copy` or `delete` with the
site's source line. A delete whose copy was refused is `skipped`. The rest of
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

The `.opt` file has ten numbered sections, always in this order, and up to
four lettered ones directly after section 4: 4a when the source declares a
`%%nooptimize` span, 4a-speed when it declares a `%%speed` region or an `%%inline`
marking, 4b in every report, and 4c under `-O1` or `-O2`. Nine of
the ten are in every report; the seventh, recursion, appears only when there
is something to say. Up to three more come last, after the statistics, under
`-O1` or `-O2`: Inline expansion when the source declares a `%%speed` region or
an `%%inline` marking, then Fall-through placement when a placement site was
recorded, then Loop unrolling when a loop was.

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
length-changing rewrite may happen" above. Nothing acts on any of it, and the
section says so. `-O=regions` prints these after the hands-off spans.

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
dump are two printers over one pass, and `Tests/Scratch` checks them against
each other figure by figure.

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
per bank with its ceiling, then one line per declared site: its bank,
address, source line and routine, and either `copied, N words, saves N us`
(with `callee deleted` when its only call was copied) or `not copied:` and
the reason in words. It is the same list as the `S1` lines of `-O=xform`.
The Transforms section, 4c, counts the T3s and T8s alone.
## The rules

Twelve rules, from the transform catalog in
`Optimizer/CompletedTasks/FeasibilityStudy.md` section 6. Every example below is taken from
the test suite in `Tools/AM1/Tests/Optimizer/`, where each rule has a source
that fires it and one source per precondition that refuses it.

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

`-O2` does not derive P5 either. It guesses, by five heuristics, and says so
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

## Testing

To run every check described below in one go, build the testing binary with
`make am1test` in `Tools/AM1` (see "The testing build"), then use the runner,
naming it: `bash Tools/AM1/Tests/run_all.sh Tools/AM1/am1test`. The binary
is required, and there is no default. The runner refuses one that is not a
testing build. It passes the binary to each of the twenty-two checks in that
check's own argument convention, and it confirms that each check actually ran
it. The twenty-second, `switches`, builds the installed kind of **am1** from
the same sources, and proves that it refuses the testing modifiers and
assembles everything else exactly as **am1test** does. It prints one table of results and exits non-zero,
naming the checks that failed, if any did. A total that differs from the
recorded baseline is marked for a person to read. It is not a failure. If you
run a check by hand, remember that a check given no argument tests the build
beside it, usually `Tools/AM1/am1test`. Pass the binary explicitly to test any other one.
`Tools/AM1/Tests/README.md` lists the twenty-two checks and how each one takes
its binary.

`Tools/AM1/Tests/Optimizer/` holds 48 sources with a stored report each: four
that exercise the word table, the decoder, the reference edges and the control
flow, twelve that each fire exactly one rule, and thirty-two that each refuse
one rule for one precondition.

```bash
./run_optimizer.sh                # the whole suite
./run_optimizer.sh 33_t1_p4       # one test
```

The runner does five checks per test, not one: the source assembles cleanly and
writes a report; the report equals the stored reference; a second run produces
the same report, which is what the sorted output is for; `-O=rules` agrees with
the report and its dump matches the rule and the polarity the file *name*
promises; and assembling without `-O` writes no report at all.

`byte_identity.sh` in the same directory is the check that matters most for
trusting the flag: it assembles every regression source with two binaries, one
without the optimizer and one with it, `-O` absent from both, and compares the
`.rim`, `.lst`, `.sym`, `.mac` and `-T` outputs plus both streams and both exit
statuses, byte for byte.

The binary without the optimizer is built from `Tools/AM1/PreOptimizer`, a
frozen copy of **am1**'s sources as they stood the moment before the optimizer
was merged. That directory is what makes the check mean anything, which is why
it must not be edited, built in place, or synced with its parent. A reference
that turns out to accept `-O` is not a reference at all -- it would be
comparing the optimizer against itself and passing every source -- so the
script probes for that and stops with an error rather than a warning, whether
the reference was built here or supplied with `-r`.

`Tests/Decoder`, `Tests/References`, `Tests/Flow`, `Tests/Routines`,
`Tests/Rules`, `Tests/Sharing` and `Tests/Regions`, in the same
`Tools/AM1/Tests/` directory, hold the acceptance checks for the individual
analyses, and
`Tools/AM1/wordtable_check.sh` checks the optimizer's word table against
**am1**'s own `-T` dump for every regression source.

`Tests/Regions/regions_check.sh` has two checks that are not about correctness
either. One is that the directive costs nothing: each region-declaring source
is assembled twice, once as written and once with the directive lines blanked
out, and all four generators' outputs must be identical with `-O` absent and
with `-O` present. The other reads the optimizer's sources for any write to
the parse tree, and requires that only `opttransform.c`, the `-O1` rewrite,
has one. Until `-O1` existed that check said there was no optimization level
at all; the promise it keeps is the same one, that the rewrite lives in one
place and nothing else in the optimizer touches the program.

Its third check is the one to run first if anything about the region design
drifts: three files, one four-word program, the directives in three different
places, and the same live finding count in all three. That is where the
misreading — a source declaring nothing losing its advice — would show first
and smallest.

`Tests/Transforms/transforms_check.sh` is `-O1`'s own suite. It has one source
per rule with a hand-rewritten twin, and each `-O1` assembly must equal its
twin in all four outputs, byte for byte. Then the test dumps must differ from
the plain assembly's in exactly the words the `-O=xform` dump says it rewrote.
Its control leg is the check to run first: the same sources with the directive
lines blanked, where `-O1` must change nothing at all, and the same for every
regression source and `adventure.am1`. It also runs Adventure with regions
declared over every span the contradiction check reports clean, and requires
the rewrite there to be exactly what it reports and stable under a second run.
Beside it, `Tests/Transforms/Probes/` runs one small program per rule on the
emulator: plain, with `-O1`, and with a deliberately wrong word, which must
give a different answer. It needs a private copy of the tree, so it is not
one of the twenty-two checks; the Transforms README says how to run it.

`Tests/Guess/guess_check.sh` holds the heuristics to hand-written loops,
handlers, buffers and absolute addresses, each with a twin they must leave
alone, and to the places in real programs they were authorized for: among them
Adventure's delay routine, cgdemo's display wait and DCS2 T12's poll loops.
`Tests/Heuristic/heuristic_check.sh` is `-O2`'s own suite. Two sources state
every fate under `-O2` and `-O1`, spans and regions included. A control leg
removes the spans, and the findings they held must then be rewritten or
guessed; another wraps a whole program in one span, which must assemble
unchanged. Over every `Tests/Optimizer` source and six programs, the test dump
must differ in exactly the rewritten words, and, where the source declares no
region or span, the findings `-O2` left alone on its guess must be exactly the
ones `-O=guess` refuses.

`Tests/Bisect/bisect_check.sh` holds the bisection modifiers to one claim: a
bisected build is the stock build with exactly the selected rewrites of the
full build in it. It does not ask **am1** which rewrites it kept. It works the
selection out from the full build's dump, and then compares every word of the
tape, the listing and the test dump, at `-O1` and `-O2`, on a hand-written
source and on Adventure. It also runs `am1bisect.sh` against a test that fails
on one chosen word, and requires it to refuse a test that never fails and one
that always does.

`Tests/Sharing/sharing_check.sh` has one check that is not about correctness at
all. Return-word sharing must never become a finding and must never fire under
any optimization level, so the first thing that script does is read the
*sources*: `optshare.c` may not name the finding list in code, and the rule
engine may not name `optshare.c`'s entry points at all. Everything else it
checks would still pass if that condition were quietly dropped.

`Tests/Routines/routines_check.sh` carries a warning worth repeating here. No
regression source contains a single `jsp`, `jda` or `cal` -- the suite tests
the assembler, not programs -- so on all thirty-four of them the call graph is
empty and every structural check over it is vacuous. What those sources prove
is that the analysis neither crashes nor invents a routine where the program
has none. What proves the rest is the suite's four hand-written sources and
`adventure.am1`, which the script picks up from the repository root when the
tree holds it, and the script refuses to report success unless the run
actually saw routines, arcs, and one cycle of each of the three classes.

**Every one of those five scripts now carries a guard of that shape**, and
each was confirmed to fail before it was accepted rather than merely to pass.
`decoder_check.sh` requires the run to have decoded a word of every
instruction group; `flow_check.sh` requires blocks, sink edges, call edges,
unreached words, every block-end kind and every return case; `refs_check.sh`
requires edges of every role and all three classification conflicts; and
`rules_check.sh` requires the *regression sweep* to have produced findings of
its own, since everything that sweep asserts is vacuously true of a source with
no findings at all.

### Decision coverage

`Tests/Coverage/coverage_check.sh` asks a different question from all of the
above. They ask whether the analysis gets the right answer. It asks **whether
any source in the repository makes it take this path at all** -- because a
code path with no source is one whose correctness rests on nothing, and its
failure mode is silence rather than a red suite.

"Covered" here means one thing and deliberately not more: some source makes the
analysis produce that outcome. Not that the outcome is right, and not that
anybody read it -- only that there is something to read. The unit is a decision
the analysis makes about a program, not a line of C; statement coverage of the
C source would answer a different question.

The outcomes counted are the enumerated ones the four dumps print -- the sink
reasons, the return cases and their reasons, the routine flags, the refusal
reasons and the classification conflicts, 45 in all -- and the list is derived
from the analysis sources themselves rather than written down in the script, so
a new enum member is measured the day it is added. `covered.txt` is a ratchet
naming the 44 that some source produces, and `uncovered.txt` holds the one that
cannot be reached together with its reason. An outcome in neither file fails:
an uncovered outcome with a stated reason is a decision, one without is a hole.

The same script also re-derives the rule-and-precondition matrix from
`optrules.c`'s own `checkShared()` call sites and requires a negative source for
every pair the code consults -- in both directions, so a rule that gains a
precondition and a negative that tests one nothing consults are both caught.
`Tests/Coverage/README.md` says what each outcome means and which source covers
it.
