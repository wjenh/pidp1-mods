# Using the **am1** optimizer advisor, `-O`

This document describes the `-O` flag and the report it writes.

Covers am1 version 1.49.\
Edit date 8-Sep-2026

## What the advisor is

`-O` makes **am1** read the program it has just assembled and write a report
naming places where the source could be shorter or faster. It is an *advisor*:

**Nothing in the assembled program is changed.** The `.rim`, `.mac`, `.lst`
and `.sym` files are byte for byte what they would have been without the flag.
Every suggestion in the report is a source change for a person to make, look
at, and test. There is no `-O1`, no level, and no rewriting; the flag adds one
output file and nothing else.

That is worth stating plainly because it is unusual. An optimizer normally
optimizes. This one does not, for two reasons that are not going away:

- A PDP-1 program can be its own data. It can patch its own address fields,
  compute jump targets, execute words in place with `xct`, and share memory
  with a device or a sequence break handler. Some of that the assembler can
  see; some of it it provably cannot.
- Most of the rewrites worth making change the number of words, which moves
  every address after them. Doing that safely means relaying out the program,
  and relayout is exactly what breaks a program that names its own addresses.

So the advisor finds the patterns, checks what it *can* check, states plainly
which check it cannot make, and leaves the decision with the author.

## Running it

**am1** [ *the usual flags* ] **-O**[**=**modifier] sourcefile

`-O` writes `sourcefile.opt` beside the other outputs. It combines with
everything else, so the ordinary way to use it is to add it to a build that
was already producing a listing:

```bash
am1 -b -d -S -O adventure.am1
```

The optional modifier adds a debug dump on **stdout**, on top of the report.
These are for working on the optimizer itself, not for daily use, but two of
them are useful for reading a program:

| Modifier | What it prints on stdout |
|---|---|
| `-O=dump` | the word table, one line per emitted word, in `-T` format |
| `-O=decode` | the same table with each word decoded and its source spelling classified |
| `-O=refs` | every reference edge, with its role, and the per-word flags |
| `-O=flow` | the basic blocks, the control-flow edges and reachability |
| `-O=rules` | the findings, one per line, live and suppressed |
| `-O=check` | runs the decoder's built-in self-check first, then proceeds |

`-O=decode` is the one to reach for when you want to know how **am1** read a
word; `-O=flow` when you want to know why something is being called unreached.

If the optimizer fails, the `.opt` file is removed rather than left half
written. The rest of the assembly is unaffected either way.

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

The `.opt` file has six sections, always in this order.

**1. Header.** The program name, the **am1** version and date, the source file
name, the statement that nothing was changed, the five preconditions, and the
P5 caveat in full. The header is the same in every report; it is there so a
`.opt` file handed to somebody else explains itself.

**2. Findings.** The suggestions, grouped by rule, and within a rule sorted by
bank and then address. The order is fixed, so two reports for two versions of
the same program diff cleanly. Each finding gives:

- the bank and address, and the source file and line
- the saving, in words, in pool or storage words, and in microseconds
- every word of the pattern: address, octal value, and the word as the source
  spelled it
- `becomes:` the suggested replacement, in **am1** syntax
- `note:` why the rewrite holds, naming the specific addresses involved

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

**4. Classification conflicts.** Words that are two things at once -- code that
is also data, code whose address field is patched at run time, code that is
written. These are not suggestions. They are the places any future rewrite
would break, and they are worth a look on their own account.

**5. Apparently unreached code.** Blocks nothing was found to reach, with the
caveat that this is a hint and not a proof. See "What the advisor cannot know".

**6. Statistics**, per bank: words by kind, the decode and spelling
breakdowns, the code/data classification, the reference edges by role, the
conflicts, the basic blocks, reachability, and a static cycle sum. A source
with no findings at all still gets a complete statistics block; that is the
half of the report that is useful when there is nothing to suggest.

## The rules

Twelve rules, from the transform catalog in
`Optimizer/FeasibilityStudy.md` section 6. Every example below is taken from
the test suite in `Tests/Optimizer/`, where each rule has a source that fires
it and one source per precondition that refuses it.

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
`law i`.

Note that the pool is shared by *value*. Two sources that both write `[0]`
name one pool word, so the pool only shrinks when every reference to that word
is rewritten.

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

**P5** -- no location involved is shared with a device, a sequence break
handler, the drum, DCS2 or a high speed channel.

**P5 is not checked, anywhere, ever.** The report says so in its header, and
it is the caveat that hangs over every suggestion in the file. The assembler
cannot know which words another agent reads or writes behind the program's
back. A load that looks redundant may be polling a location a device updates.
A word that looks dead may be a mailbox. And timing is the same kind of thing
in reverse: a delay loop for the typewriter, the punch or the display is meant
to take the time it takes, so making it faster breaks it.

Only the author can settle P5, and the advisor does not pretend otherwise.

## What the advisor cannot know

Two of the report's sections are heuristics, and both say so where they print.

**Unreached code is a hint, not a proof.** A block appears there when nothing
was found that reaches it. Several ordinary constructions produce false
entries:

- A dispatch table of `jmp` words is read as data and never followed as a
  jump, so a routine entered only through one looks unreached.
- Anything reached only through a pointer that could not be followed, or
  through an address field a `dap` writes, looks unreached.
- **The commonest cause by far is the inline argument.** A call whose argument
  word follows the call, and a callee that returns past that word, loses its
  return edge: the return is assumed to go to the word after the call, that
  word is the argument, the argument is data, and so everything the caller
  would have reached afterwards is called unreached. Proving otherwise means
  looking inside the callee, which nothing here does.

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

`Tests/Optimizer/` holds 48 sources with a stored report each: four that
exercise the word table, the decoder, the reference edges and the control flow,
twelve that each fire exactly one rule, and thirty-two that each refuse one
rule for one precondition.

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
built from the stock **am1** sources and one with the optimizer in it, `-O`
absent from both, and compares the `.rim`, `.lst`, `.sym`, `.mac` and `-T`
outputs plus both streams and both exit statuses, byte for byte.

`wordtable_check.sh` checks the optimizer's word table against **am1**'s own
`-T` dump for every regression source.
