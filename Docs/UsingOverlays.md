# Using drum overlays in **am1**

This document describes drum overlays, parts of one **am1** program that take turns in the same core,
kept on the Type 23 drum and loaded when they are called.
It covers the directives, the overlay manager the program includes, the loader **ovlload**, and what
each costs.
*UsingAM1.md* is the assembler itself and *UsingType23Drum.md* is the drum.

This is version 1.3 and covers up through am1 version 5.1; it will be updated as needed.\
Edit date 21-Oct-2026
Add %%ovload

## What are overlays?

The PDP-1 has no relocatable code, so code brought in from the drum must already be assembled for the
core it will run in.
With overlays, **am1** does that for you:

- a **window** is a fixed range of core in one bank, declared once and kept free of everything else;
- an **overlay** is a named section of the program assembled to run in a window.
  Several overlays share a window, and one at a time is in it;
- the **root** is everything else: the program as it always was, loaded from its tape and always in core.

**am1** writes the root to the tape as usual and the overlays to a separate file, *file.ovl*.
**ovlload** puts the overlays on the drum and loads the tape.
At run time, a small **overlay manager** in the root reads an overlay into its window when it is
called and is not already there, then calls it.
An overlay may also be data only, brought in by *%%ovload*, which calls nothing and gives the root
the address of the data.

A program may have several windows, in the same bank or in different ones, and an overlay in each
can be in core at the same time.

Two rules shape everything else:

- **Only the root calls or loads an overlay.** An overlay may use any root routine or word, but it may
  not call or name another overlay, in its own window or any other.
- **An overlay is read-only unless it says otherwise.** Each load starts from the words **am1**
  assembled; what it changed while it ran is lost when another overlay takes the window.
  State that must last belongs in the root, or the overlay is declared *persistent* and is written
  back to the drum before it is replaced.

## An example

```
ovldemo: two overlays sharing one window
%%drumreserve 20                // Adventure's save track: overlays keep off it

bank 1
100/
calls,  0                       // a root word of bank 1, outside the window

%%window work, 2000, 3777       // bank 1, 02000-03777: the root may not use it

%%overlay plus, work
        export plusRun
plusRun, dac plusX              // AC is the return, IO the argument
        dio plusArg
        idx calls               // a root global of this bank, by its name
        lac plusArg
        add [100]               // pooled in the overlay, at %%endoverlay
        eem
        jmp i plusX             // the result in AC
plusX,  0
plusArg, 0
%%endoverlay

%%overlay minus, work
        export minusRun
minusRun, dac minusX
        dio minusArg
        idx calls
        lac minusArg
        cma
        eem
        jmp i minusX
minusX, 0
minusArg, 0
%%endoverlay

bank 0
100/
#include <DRUM/overlay.ac>      // the manager, before the first %%ovcall

go,     jsp ovInit              // ready for a start; extended mode on
        lio [5]
        %%ovcall plus, plusRun  // loads plus: AC 105
        dac r1
        lio [5]
        %%ovcall minus, minusRun // evicts plus, loads minus: AC -5
        dac r2
        lio [6]
        %%ovcall minus, minusRun // minus is resident: no transfer
        dac r3
        hlt

r1,     0
r2,     0
r3,     0
        constants
%%ovtable
        start go
```

Assemble it and load it:

```
am1 ovldemo.am1
ovlload ovldemo.ovl
```

**am1** writes *ovldemo.rim* (the root) and *ovldemo.ovl* (the two overlays).
**ovlload** writes the overlays to the drum, then loads *ovldemo.rim* into the running pidp-1 as
**fastload** does, and offers to start it.
*fastload ovldemo.rim*, or **ad1**'s *load ovldemo.rim*, does the same, since a tape with a *.ovl*
beside it has its overlays written too (see "Loading with fastload or ad1").

## The directives

Like every directive special to **am1**, these are spelled with a leading *%%*, so the plain words are
still ordinary names.
Numbers are in the current radix, as everywhere else.

### %%drumreserve

```
%%drumreserve 0-17, 20
```

Tracks the overlays may not use: single tracks and ranges, tracks 0 to 37 octal.
It may be given more than once, and the lists are combined.
It is your statement of what else is on the drum (your program's own data, Adventure's save track,
20 octal, a Z-machine story), and it affects only where **am1** puts this program's overlays.

### %%window

```
%%window name, first, last
```

Declares a window in the current bank, from *first* to *last* inclusive.
From then on a root word placed in that range is an error, with or without *-M*: code, a table, text,
a constants or variables pool, all of them.

A window must hold at least three words (the two header words and one more), must stay in its bank,
must not overlap another window of the same bank, and in bank 0 must end at or below 07750, as all
bank 0 code must.
Its addresses may not be tagged with another bank.

### %%overlay and %%endoverlay

```
%%overlay name, window
%%overlay name, window, persistent
        ...
%%endoverlay
```

Everything between the two is the overlay.
It is assembled in its window's bank, whatever bank is current where it stands, and *%%endoverlay*
returns to that bank; no *bank* is written around an overlay.
Its location counter starts at the window's first address plus 2, after the header.

Inside an overlay:

- **Labels, *var*s, constants and variables are the overlay's own.**
  A *constants* or *variables* inside the overlay places its pool there, and any still pending at
  *%%endoverlay* are placed then, constants first.
- **A name the overlay does not define is its bank's root global of that name,** whether the root
  defines it before or after the overlay.
  A root global of another bank is reached as usual, as *name:N* under extended mode
  (*UsingAM1.md*, "Bank and cross-bank references").
- **An overlay's label may not have the name of a root global of its bank.**
  It would be the usual redefinition, and it keeps root names unambiguous inside every overlay.
- ***export*** names the symbols the root may reach: the entry points it calls with *%%ovcall*, and the
  data it loads with *%%ovload*.
  An overlay may export any number of them, and exports only symbols it defines itself.
- *local* scopes work as they do anywhere, but one may not be open across *%%overlay* or
  *%%endoverlay*.
- Not allowed: *bank*, an origin (*xxx/*), *import*, *%%window*, *%%ovtable*, *%%ovcall*, *%%ovload*,
  and another *%%overlay*: overlays do not nest.

An overlay must begin and end in the same file.

Window names and overlay names are two namespaces of their own, apart from the program's symbols, so
an overlay may be called *plus* and a label *plus* too.

### %%ovcall

```
        %%ovcall overlay, entry
```

The way the root runs an overlay's code; *%%ovload*, next, brings in its data.
*entry* must be a symbol the overlay exports.
It assembles to the manager's call and two data words, chosen by **am1** so you need not know where
the manager is:

- from the manager's own bank, 3 words: *jsp ovEnter*, then the two data words;
- from another bank, 4 words: *eem*, *jsp i [ovEnter:B*, then the two data words, the literal pooled
  in the calling bank and shared by every such call there.

The data words are the full address of the overlay's entry in the table and the entry's full address.

*%%ovcall* is allowed in the root only.
The manager, *overlay.ac*, must be included before the first *%%ovcall*, so that the call can be
made in the right shape.

Like *table* and *%%optimize*, it cannot share its line with a label; put the label on the line
before:

```
sPlus,
        %%ovcall plus, plusRun
```

### %%ovload

```
        %%ovload overlay, symbol
```

The same as *%%ovcall*, except that nothing is called: the overlay is made resident, read from the
drum only if it is not already in its window, and the manager returns with the full address of
*symbol* in AC.
It is for an overlay that is data, a *type340* display list or a table, though it works on any
overlay.
*symbol* must be a symbol the overlay exports, and an overlay may export several, so one data
overlay can hold several lists.

It is the same size as *%%ovcall*, calls the manager's *ovFetch* in place of *ovEnter*, and has the
same two data words, the second the symbol's full address.
The rules are *%%ovcall*'s: the root only, after *overlay.ac*, and no label on its line.
After it:

- AC holds the symbol's full address, and IO is as it was;
- extended mode is on, so the data is reached through the address with an indirect:

```
        %%ovload pics, picBoat  // AC is picBoat's full address
        dac picP
        lac i picP              // picBoat's first word
```

The address is good until another overlay takes the window.
A *persistent* data overlay keeps what the program writes into it across evictions, as any
persistent overlay does.
*%%ovload* is refused while an overlay's entry is running, as *%%ovcall* is (see "When the manager
halts").

### %%ovtable

```
%%ovtable
```

Where **am1** puts the overlay table, in the root, in any bank.
It is required when the program has an overlay, and it must come after every *%%window* and
*%%overlay*, so its length is known where it stands.
**am1** labels it *ovTable*, a root global of its bank, which the manager finds.

## Calling and writing an entry

An entry is an ordinary cross-bank *jsp* routine, in the shape of *memory.ah*'s *farjsp* and
*farrtn*:

- it is entered by *jsp i* with extended mode on: AC holds its return, IO the argument the caller had
  in IO;
- it returns with *eem* and *jmp i* through the return it saved, its results in AC and IO;
- AC at the *%%ovcall* is lost, as with any *jsp*.

The caller sees extended mode on after the call, whatever it was before.

An entry may call root routines.
It may not, directly or through a root routine, reach another *%%ovcall* or a *%%ovload*: the manager
stops at *ovErrNest* (see "When the manager halts").

## The overlay manager

The manager is *Am1Includes/DRUM/overlay.ac*, with its defines in *overlay.ah*:

```
#include <DRUM/overlay.ac>
```

Include it once, in the root, in any bank, before the first *%%ovcall* or *%%ovload*.
Its literals go into the including bank's next *constants*.
It uses plain PDP-1 instructions only, so it runs on any configuration.

It defines:

- *ovEnter*, the entry *%%ovcall* calls, and *ovFetch*, the entry *%%ovload* calls;
- *ovInit*, to call at the program's start (*jsp ovInit* from its bank, *farjsp* from another).
  It clears *ovActive* and *ovRetries*, so that a program stopped inside an overlay, or at one of the
  manager's halts, can be started again.
  It leaves the table's record of what each window holds, since core still holds it.
  Loading the program's tape again resets that record to "nothing", so every overlay is then read
  again at its first call.
  It leaves *ovErrHook* as the program set it;
- *ovActive*, set while an entry runs;
- *ovRetries*, the number of transfers made again since *ovInit*;
- *ovErrHook*, 0 unless the program sets it, where the manager goes on an error instead of halting
  (see "When the manager halts");
- *ovErrNest*, *ovErrStamp* and *ovErrDrum*, the halts, and *ovErrSwap* with *OV_SINGLE_PASS*
  (see "The single-pass swap").

On each *%%ovcall* the manager does the following; on each *%%ovload* it does the same, but returns
the symbol's address in place of calling an entry:

1. stops at *ovErrNest* if an overlay is running;
2. finds the overlay in the table, and calls the entry at once if the overlay is already in its window;
3. if a *persistent* overlay is in the window, writes it back to its track first, or, with
   *OV_SINGLE_PASS*, swaps the two in one transfer (see "The single-pass swap");
4. reads the overlay into its window, waiting for the drum to be idle before it starts, so a transfer
   of your program's own finishes first, and waiting for its end;
5. makes a transfer that ended in error again, up to *OV_TRIES* transfers in all (4 unless the
   program sets it, below), each one after the first counted in *ovRetries*, and stops at
   *ovErrDrum* if the last of them fails too.
   An ad1 stop, the STOP switch or a single step during a transfer ends it in error, so a load
   interrupted that way simply happens again.
   A swap is never made again: one that ends in error stops at *ovErrSwap*;
6. checks the overlay's header against the table, and stops at *ovErrStamp* if the drum holds another
   build's overlays;
7. records the overlay as resident, sets *ovActive*, calls the entry, clears *ovActive*, and returns.

The window's record is cleared before a read and set only once the header is checked, so a halt
during a read leaves the window recorded as holding nothing.

Each "stops at" above is a halt unless the program has set *ovErrHook*; then the manager goes there
instead (see "When the manager halts").

To change the number of tries, define *OV_TRIES* before the manager is included, and before anything
else includes *DRUM/overlay.ah*:

```
#define OV_TRIES 8
#include <DRUM/overlay.ac>
```

### Sequence breaks

Every drum transfer ends with a sequence break request on the drum's channel, the manager's included
(*UsingType23Drum.md*, "Interrupts").
The manager leaves breaks as it finds them and polls the drum's status itself.
A program that runs with breaks on needs a handler for the drum's channel.
One that keeps them off and turns them on later should issue a *cbs* first, or it will take a break
for the manager's last transfer.

### Your program's use of the drum

The manager waits for the drum to be idle then makes its own transfers.
Your program can use the drum also, with these cautions:

- reserve tracks with *%%drumreserve* so no overlay is put there;
- a *dra* read after an *%%ovcall* reports the manager's transfer, not yours.

## Read-only and persistent overlays

A **read-only** overlay, the default, is never written back.
Each load starts from the assembled data.
Its *var*s and other words start with the original values at every load, unlike those in core memory.
A *var* in a read-only overlay gets an *overlay* warning.

A **persistent** overlay, *%%overlay name, window, persistent*, is written back to its drum location
when another overlay is going to to be loaded into its window.
Any changes made while in memory are preserved.

A halt during the swapping can leave both the memory version and the drum version in an incosistent state.

### The single-pass swap

The drum can write one track and read another in the same transfer.
A persistent overlay can be written back and the next one read in one transfer, a swap, which
saves a complete revolution of the drum, 34.8 ms, on every swap.
This is the default behavior.

Define *OV_TWO_PASS* before the manager is included to use a two-part update, separate writes and reads.
This can cost an extra drum rotation, it's not usually used.

```
#define OV_TWO_PASS
#include <DRUM/overlay.ac>
```

Nothing else changes, the same directives, the same *.ovl*, the same **ovlload**.
Swapping a read-only overlay is still a single read, since nothing is written back for one.
The swap moves as many words as the longer of the two overlays.
Space is always reserved on the drum for each overlay using the size of the largest in the window,
there is no danger of overrunning an overlay's storage.

**The price of a halt.**

An ad1 stop, the STOP switch, or a single step part-way through a swap ends it in error, but by then
the window holds the incoming overlay's first words and the outgoing one's after them, while the
outgoing overlay's track holds its new words up to the same point and the words of its previous
eviction after it.
Nothing records where that point was, so the outgoing overlay's words cannot be put back together;
a swap is never made again.

Making it again would write the incoming overlay's words over the outgoing one's track.
Instead, the manager stops at *ovErrSwap* with 4 in AC, the window recorded as holding nothing
or calls the error hook if defined.

Continue reads just the incoming overlay, an ordinary read, and the program goes on.
The outgoing overlay's next load gets its track as the swap left it, part new and part old.
Starting the program again from its start address after such a halt instead of continuing loses the
outgoing overlay's words the same way.

Use persistend overlays for a program you will not stop while it is swapping, or whose
persistent overlays can stand losing their words after a stop.
Take carw while debugging with ad1.

## The drum layout

**am1** places every overlay itself.

- Each overlay gets its window's entire  range on one track at drum addresses equal to the window's core
  addresses.
  All the overlays of one window are at the same drum addresses but on different tracks.
- Tracks are given out in source order, the lowest free first, skipping the reserved ones.
  Windows whose ranges do not overlap can share a track, two windows with the same addresses but in different
  banks cannot.
- A window can have as many overlays as there are tracks allowed, up to 32.
  If no more can be assigned, **am1** reports which one failed.

*-S* prints the drum layout after the per-bank high-water marks; the listing has it aft3er *%%ovtable*:

```
Overlays on the drum, stamp 110524:
overlay plus, number 1: track 0, drum 2000-3777, window work in bank 1, 14 words, read-only
overlay minus, number 2: track 1, drum 2000-3777, window work in bank 1, 13 words, read-only
```

The word counts include the two header words.

## When the manager halts

Each halt stops with its number in AC:

| Label | AC | Cause | Continue |
|-------|----|-------|----------|
| *ovErrNest* | 1 | an *%%ovcall* or *%%ovload* while an overlay is running | halts again |
| *ovErrStamp* | 2 | the overlay read is not this build's, or not this overlay, or none is loaded | reads the overlay again, so run **ovlload** and continue |
| *ovErrDrum* | 3 | *OV_TRIES* transfers in a row (4 by default) ended in error | tries as many again |
| *ovErrSwap* | 4 | with *OV_SINGLE_PASS* only: a swap ended in error, a halt part-way through it; the outgoing overlay's words are lost | reads the incoming overlay alone |

The stamp is what makes the second one useful, a root reassembled and the drum not reloaded, or the
opposite, stops at the first load instead of running the wrong code.

The numbers are *OV_ERR_NEST*, *OV_ERR_STAMP*, *OV_ERR_DRUM* and *OV_ERR_SWAP* in *overlay.ah*.

### Not halting: the error hook

A program that must not stop, one serving users over a terminal line for example, sets *ovErrHook*
to the full address of its own recovery point after *ovInit*:

```
        lac [recover:.]         // the full address of the recovery point
        dac i [ovErrHook:*]     // with extended mode on
```

On any of the errors the manager then jumps to it, by *jmp i*, instead of halting.
The program decides whether to report the error, try the work again, or carry
on without the overlay.

At the recovery point:
- AC holds the error number, as at the halt;
- IO holds the return address of the *%%ovcall* or *%%ovload* that failed, past its two data words,
  as a *jsp* leaves it so its low 16 bits are the full address;
- *ovActive* is 0, so the next *%%ovcall* or *%%ovload* is allowed.
  After a nesting error this abandons the overlay that made the call and every return pending
  inside it;
- extended mode is on;
- the window's record is what the error left, nothing, after a failed read, a failed swap or a stamp
  mismatch.
  After a failed write-back of a persistent overlay it  names the overlay whose words are
  in core; after a nesting error it names the overlay that was running.

A later *%%ovcall* reads the overlay again when it needs to.
A recovery point that retries the same call after a stamp error will fail again.
So count the tries and giv up, or tell the user to run **ovlload**.
Setting *ovErrHook* back to 0 restores the halts.

## What it costs

**Core:**

- the manager: space in its bank's pool, larger with *OV_TWO_PASS*
- the table: 1 header word, 1 per window, and 4 per overlay
- each *%%ovcall* or *%%ovload*: 3 words from the manager's bank, 4 from another, plus one literal
  in each other bank shared by the same calls
- each overlay: the 2 header words at the start of its window
- each window: its entire range

**Time,** for a load of n words, the header included:

- the drum rotating to the window's address, half a revolution on average, 17.4 ms, at most
  34.8 ms; an address the drum reaches within the 250 us after the *dcl* waits an entire revolution
- the transfer, n x 8.5 us: 1,024 words take 8.7 ms, a whole bank 34.8 ms.

A 1,024-word read-only overlay loads in about 26 ms on average and at most about 44 ms.
Swappng a persistent overlay in *OV_TWO_PASS* mode  adds its write-back and most of a
revolution between the two transfers, about 61 ms in all for 1,024 words.

Without *OV_TWO_PASS* it is only one transfer of the longer overlay's length, the time is about
26 ms for 1,024 words.

A call to an overlay already in its window does no transfer, but the manager's checks are not free.
Each call then takes about 0.6 ms in a program with two windows.
Finding the overlay in the table is a linear search, so more windows add more time.

Overlays suit coarse phases with each call doing a significant amount of work or one that has
a long runtime, not inner loops.

## Assembling

A program without an overlay assembles exactly as it did before.
With overlays:

- **The tape**, *file.rim* or *file.bin*, holds the root only, not any overlay code.
- ***file.ovl*** is created along with the primary rim or bin file.
- ***file.osym*** is created in additon to the *.sym* file when *-s* is given.
- **The listing**, *-l*, lists each overlay under its own heading with its two header words.
  Its addresses use its window's address.
  Each *%%ovcall* and *%%ovload* is listed with the table with what it holds.
- **Test mode**, *-T*, dumps each overlay word with the overlay's name after it:
  *bbaaaa vvvvvv name*.
- ***-O*, *-O1*, *-O2*** and *-O=source* work with overlays.
  The root is optimized first, then each overlay in turn with the root as context, inside its window.
  See "Programs with drum overlays" in *UsingTheAm1Optimizer.md*.
  The *.ovl* holds the rewritten overlays and a new stamp, load it again after an optimized build.
- ***-d*** writes the listing, the *.sym* and the *.osym*, as *-l* and *-s* do, for **ad1**.
- ***-m*** writes **macro1** source with the overlays annotated, as banks are.
  It is not runnable.

The *overlay* warning, for a *var* in a read-only overlay, is on by default; *-W=-overlay* turns it
off.

### The .ovl file

Text, so it can be read and compared, every number octal:

```
%%am1 overlay file%%
V1
ovldemo.am1 stamp 110524 table 000365
overlay plus window work bank 1 first 2000 last 3777 track 0 length 14 mode ro
110524 000001 242011 322012 440100 202012 402013 724074
612011 000000 000000 000100
overlay minus window work bank 1 first 2000 last 3777 track 1 length 13 mode ro
...
```

The third line gives the source's name, the build stamp and the full address of the table on the
tape.
Each overlay's line gives its window, bank, track, length (header included) and mode, *ro* or *rw*
(persistent), and its data words follow, 8 to a line, the header first.

### The .osym file

The overlays' own symbols in the *.sym* line format with the overlay's
name added:

```
%%am1 overlay symtab file%%
V1
ovldemo.am1
012002 X plusRun 12 plus
012011 G plusX 19 plus
...
```

The *.sym* itself is unchanged and holds the root's symbols only, so *import* is not affected.
**ad1** reads the both the *.osym* and the *.ovl* files, see "Debugging".

## Loading: ovlload

```
ovlload [-i drumfile] [-c] [-n] [-t tape] [-h host[:port]] [-y] [-m] [-M memfile] file.ovl
ovlload -p tapefile [-n | -t tape] file.ovl
```

**ovlload** checks the entire *.ovl* first, then writes each overlay on its track at its window's
drum addresses with the rest of the window's range filled with zeros.
It then loads the program's tape as **fastload** does.

- *-i drumfile*: the drum file, by default */opt/pidp1-mods/pdp23drum*
  It is written in place, a missing file is created and a short one extended with zeros.
- *-n*: write the drum only, load no program; no tape is read, so the stamp is not checked.
- *-t tape*: the tape to load; by default the *.ovl* name matching the *.rim or *.bin* name.
- *-h host[:port]*: the pidp-1 to load into over the debugger link, by default
  *localhost:1044*.
- *-y*: start the program once it is loaded, without asking.
- *-m*: load into the core-memory file instead, */opt/pidp1-mods/coremem*, asking first, just as
  **fastload** *-m* does; *-M memfile* names the file and implies *-m*.
- *-c*: compare only (below).
- *-p tapefile*: punch a paper tape for the pdp-1 to write its own drum from (below).

**Before anything is written,** the tape's word at the table's address must be the *.ovl*'s stamp,
**ovlload** never loads a drum and a program from different builds.
There is no override, assemble again and use the matching pair.
With *-n* no tape is read and nothing is compared.

Over the debugger link the pidp-1 is stopped before the drum is written.

**Existing data** on a track it writes is written over, without asking.
If you want to preserve the current drum, copy it.

*%%drumreserve* keeps a program's overlays off tracks you have given to something else.

**ovlload** can run while the emulator runs, the drum plugin rereads the file when
**ovlload** closes it.
With *-n* the pidp-1 is not stopped, so make sure no program is using the drum.

### Loading with fastload or ad1

**fastload** and **ad1**'s *load* write a program's overlays too when the tape has a *.ovl*.
They use **ovlload**'s own code, so the checks, the drum writes, and the warnings are the same:

- With *-h* naming another machine they load the tape but not the drum.
- **ad1** in test mode, *-T*, writes no drum data.
- **fastload** *-m* writes the drum then the core-memory file.
- A tape with no *.ovl* loads as it always has.

**ovlload** is still the tool for writing the drum alone (*-n*), comparing it (*-c*), loading
another tape than the *.ovl*'s (*-t*) and punching a paper tape (*-p*).

It prints one line per overlay:

```
overlay plus: track 0, drum 2000-3777, 14 words, ro
overlay minus: track 1, drum 2000-3777, 13 words, ro
```

### Is the drum current? -c

```
ovlload -c [-i drumfile] file.ovl
```

Compares the drum with the *.ovl* and writes nothing, for a script to ask whether the drum is current
for this build.
A read-only overlay is compared word for word over its window, padding included; a persistent one by
its two header words only, since the manager writes its words back.
The first difference is reported as the track, the drum address, the drum's word and the file's word.

### Exit status

0 done, or with *-c* the drum is current; 1 with *-c* a difference; 2 an error, nothing having been
written unless the message says so.

### What is on the drum: drumlist

```
drumlist [-i drumfile] [file.ovl ...]
```

**drumlist** lists the drum's tracks: a program **drumupdater** loaded, with its label and start
address; Adventure's tracks; a Z-machine story's tracks, the first with the story's version and
serial; otherwise "not initialized".
An overlay cannot be recognized from the drum alone, so give the *.ovl*: each of its overlays is then
listed on its track, after anything else on that track, with what *ovlload -c* would find for it.

```
Track 1: overlay one of prog.ovl, drum 0400-0437, window wa in bank 1, 4 words, read-only, current
Track 3: overlay two of prog.ovl, drum 0400-0437, window wa in bank 1, 3 words, persistent, current
```

**drumlist** numbers tracks in decimal, as it always has; the addresses and lengths are octal, as in
the *.ovl*, and **ovlload** gives tracks in octal.
The last field is *current*, *differs* (a word of a read-only overlay, padding included), or *stamp
differs* (a header word: another build's overlay, or none).
More than one *.ovl* may be given; one that cannot be read is reported and left out.
**drumlist** writes nothing; *ovlload -c* is the check for a script.

### A paper tape for the pdp-1: -p

```
ovlload -p tapefile [-n | -t tape] file.ovl
```

*-p* punches a paper tape instead of writing a drum file.
The pdp-1 reads it in and writes the overlays to the drum, then, withut -n, loads the program and starts it.
The tape holds am1's loader, a small durm writer program, each overlay with its window's range and a
checksum, and then, unless *-n* is given, the program tape.
The *.ovl* is checked as always and, without *-n* the stamp is also checked.
*-i*, *-c*, *-h*, *-y*, *-m* and *-M* mean nothing for a tape and are refused.

```
Tape prog.tape: the writer at 6104-6777, the nop loader.
overlay one: track 20, drum 7000-7740, 4 words, ro, checksum 261621
overlay two: track 21, drum 7000-7740, 5 words, rw, checksum 730630
Punched, read it in and it will start the program when the drum is loaded.
```

With *-n* only the drum is loaded.

Load it using any of the usual ways a program tape is loaded.
Once the drum is written, the loader reads the program and starts it with no halt, as the program's
own tape would, however a program assembled with a stop directive halts as usual.
With *-n* there is no program, the pdp-1 halts with AC 0 when the drum has been written.
Every halt of the writer has AC telling why:

| AC | Meaning | Continue |
|---|---|---|
| 0 | with *-n* only, the drum has been written | halts again |
| 1 | an overlay's words do not match its checksum: a bad frame | should never happen, the file was corrupted |
| 2 | an overlay read back from the drum differs from core | writes it again |
| 3 | a drum transfer failed 4 times | tries 4 more |

Before it writes anything, the writer looks at the tracks the tape writes and types a line on the
typewriter for each track holding another tool's data which it then writes over, as **ovlload**
does:

```
track 20 held the adventure save, written over
track 21 held a drumupdater program, written over
track 01 held part of a z-machine story, written over
```

A story whose header gives no length counts as the whole drum, so the writer may warn of a track
the story never reached; **ovlload** looks for the story's end and does not.

The writer is 0674 words, in the highest run of bank 0 from 0100 to 07750 that no bank-0 window
covers; words 0-077 are left alone, for a program's 16-channel break locations.
The program is not in core while it runs, and may load over it afterwards.
A program whose bank-0 windows leave no such run, or with a bank-0 window reaching 07751, the
loader's own words, gets no tape.
The loader is the *eem* one when any window or the program needs extended memory.

am1's loader has no checksum of its own, so a bad frame in a block's address words puts the block in
the wrong place unseen; the checksum catches a bad frame among an overlay's words.

## Debugging

**ad1** (v2.1 on) reads the *.ovl* and the *.osym* when it opens the program, and shows a window's
addresses with the symbols of the overlay in the window now, read from the table in core for each
command. An overlay's symbol can be named whether its overlay is in or not; a name that several
overlays define is given as *name@overlay*. A breakpoint hit in a window names the overlay. From
v2.3, a breakpoint set as *name@overlay*, e.g. *break edGo@edit*, is hit only while that overlay is
in its window, so one overlay of several sharing a window can be debugged without stopping for the
others. From v2.4, with an emulator from 13-Oct-2026 on, the emulator keeps that binding as a
condition on the window's word in the table, so the other overlays' passes do not halt the machine
at all, and a watch can be bound the same way, *watch edCount@edit*, unmoved by the other
overlays' writes and by the window's loading. From v2.8, *load* writes the program's overlays to
the drum along with its tape (see "Loading with fastload or ad1"). *Using AD1* has the details,
under "Programs with drum overlays".

Which overlay is in a window is in the table: *ovTable* is the stamp, the next word is the first
window's resident overlay number (0 for none), then the next window's, in the order the windows were
declared. Overlays are numbered from 1, in source order, as *-S* prints them.

*ovRetries* counts the transfers the manager has had to make again, so a nonzero value after a run
you never stopped points at the drum.

## The table and the header

For reference; *overlay.ah* defines each offset and field by name, and **am1** builds them to the
same definitions.

The table, at *ovTable*:

- the stamp, an 18-bit hash of every overlay's words and the table's other words, never 0;
- one word per window, in window order: the number of the overlay in it, 0 for none;
- four words per overlay, in overlay order:
  - the *dia* word of its read: the read bit, its track, its drum address (the window's first address);
  - the *dwc* word of its read: its length, header included;
  - the *dcl* word: the window's bank and first address;
  - the full address of the window's word in the table, with bit 0 set for a persistent overlay.

The header, the first two words of every overlay: the stamp and the overlay's number.

## What am1 checks

Each of these is an error, with its line:

- an *%%overlay* inside an overlay or a local scope, naming an undeclared window, or with a third
  operand other than *persistent*; an *%%endoverlay* with none open; an overlay never closed, or
  crossing a file boundary;
- inside an overlay: *bank*, an origin, *import*, *%%window*, *%%ovtable*, *%%ovcall*, *%%ovload*, or
  a local scope still open at *%%endoverlay*;
- an overlay longer than its window, header included;
- a window whose first address is above its last, shorter than 3 words, outside its bank, above 07750
  in bank 0, tagged with another bank, overlapping another window, or holding a root word;
- a window or overlay name declared twice;
- a reference from an overlay to another overlay's symbol, or from the root to any overlay symbol
  except through *%%ovcall* or *%%ovload*;
- an overlay label, *var* or *export* with the name of a root global of its bank, or an *export* of a
  symbol the overlay never defines;
- an *%%ovcall* naming an overlay that does not exist, an entry it does not export, or coming before
  *ovEnter* is defined;
- a *%%ovload* likewise, its symbol in place of the entry and *ovFetch* in place of *ovEnter*; with
  an *overlay.ac* older than *%%ovload*, which has no *ovFetch*, the message says so;
- a program with overlays and no *%%ovtable*; an *%%ovtable* given twice, before any overlay, or
  followed by a *%%window* or *%%overlay*;
- a *%%drumreserve* track outside 0-37, or a range running backward;
- more overlays than free tracks;
- an *%%overlay* or *%%endoverlay* inside an *%%optimize* region, a *%%nooptimize* span or a *%%speed*
  region, with or without *-O*: a region may not cross an overlay's boundary (from am1 v4.0).

## Not yet

- An overlay calling another, with a return stack.
- Overlays read from a microtape, or several *.ovl* files on one tape.
