# Type 550 Microtape -- tests

Two kinds of test live here. The host tests (`mttest`, `plugintest`,
`roundtrip.sh`) need no emulator. `mttest` links the two core files
(`../control550.c`, `../transport555.c`) into a host program and drives
them with synthetic time, the same way the I/O poll and the IOTs do.
`plugintest` builds the reader plugin itself (`../../Reader/IOT_2.c` with
`../microtape.c` and the core) into a host program and calls its entry
points as the IOT loader would. The pattern follows
`IOTs/TestGateway/Tests/hscharness.c`. The Phase 5 probes (the `.am1`
files) run on the emulator; see "Emulator probes" below.

`IOTs/install.sh` skips directories named `Tests`, so nothing here is
built or installed by it.

## Running

    make              # builds and runs mttest
    ./mttest -v       # also prints the deadline windows the model gives
    make plugintest   # builds and runs plugintest
    make roundtrip    # builds mkmicrotape and runs its tests
    make clean

Each test program prints one PASS/FAIL line per group, every failed
check, and a summary, and exits 0 only if every check passed. `mttest`'s
image-file groups create and remove `mttest-*.img` in the current
directory. `plugintest` is built with its base directory set to
`plugintest.d` in place of `/opt/pidp1-mods`, and creates and removes
that directory; the warnings it prints on stderr are for the bad lines
and names it feeds the plugin on purpose.

## What mttest covers

The references are to `Magtape/TASK-TYPE550.md` (sections 5.2 and 5.3)
and to `Magtape/MICROTAPE-RESEARCH.md` (section 3.5, the program
deadlines). The write enable, D256 latch and All Halt groups, and the
deadlines as they now stand, come from `MiscTasks/Completed/TASK-TAPE-HALT-WRITE.md`
(13-Sep-2026), which matched the model to DEC's documents: H-550 (1965),
the DECUS 1963 paper and the F-03 brochure.

At 13-Sep-2026: `mttest` 337 checks, `plugintest` 68, `roundtrip.sh`
20, all passing.

| Group | Checks |
|---|---|
| 5.2 IOTs | power-on status; `mlc` refused with no unit or no tape (UNABLE + ERF, one break); unit numbers 1-7, 010 = 8, 0/011-017 = none, IO bits 0-1 and 6-17 ignored; `mwr`/`mrd` move the 18-bit buffer and clear only DF/BEF; `mse` clears ERF and UNABLE; modes 4-6 run the tape with no flags and leave it untouched; mode 7 blanks the reel (a written block reads blank and every block checks), raises no flags and runs the tape; mode 7 on a locked unit is refused and the tape is intact; REV and GO status bits; MTE never set |
| 5.3 search and delays | first search flag from the load position to the nanosecond, at the end of block 0's space 0 (the block mark); one flag per block, 52.8 ms apart; search words `260000\|block` and `450000\|block`; start delay (block marks passed during the 200 ms ramp raise nothing); turnaround delay (300 ms), the first reverse flag at space 263 (the reverse block mark); selection delay (34 ms, with a control leg that reselects early enough); reselecting the same unit starts no delay; 150 ms stop over 375 slots |
| 5.3 read | forward: DF at the end of slots 3-259, BEF at 260, 200 us apart, one break per flag, words as stored; reverse: the same block last word first (decision D3); both total -0 |
| 5.3 write | entering write raises DF at once; DFs at the ends of slots 3-257, BEF at 258; -0, the data and the checksum land in slots 3, 4-259 and 260; block totals -0; neighbors untouched; one write-through flush; reverse write mirrors the slots; forward write / reverse read and reverse write / forward read identities; a partial rewrite leaves the block failing its checksum |
| 3.5 block-end deadlines | read BEF -> next DF at 1.4 ms (hit at 1.3 ms, MISS at 1.4 ms with the buffer overwritten); write BEF -> next DF at 1.6 ms; a write BEF unanswered gives MISS at the checksum's interchange, 200 us after the BEF, and the writers go off first: the checksum space and the next block's leading -0 keep what the tape held, the block fails, the next DF still comes at 1.6 ms and the tape keeps moving; read BEF -> search (700 and 799 us hit, the flag 800 us after the BEF; 801 us misses a block); write BEF -> search (1100 and 1199 us hit, 1201 us misses); read BEF -> write (1.15 ms writes the next block correctly, 1.25 ms does not) |
| 3.5 search-flag deadlines | search -> write at 150 and 390 us writes the block correctly, with the control's -0 at the lock (the end of space 2, 400 us after the flag); at 410 us space 3 keeps what the tape held, word 1 still lands in space 4 and the block fails; the first `mwr` at 590 us is in time, at 610 us MISS and nothing is written after the lock's -0; search -> read at 390 and 599 us catches the leading checksum, its flag 600 us after the search flag; at 601 us the first word is data word 1 |
| 3.5 word deadline and MISS | read: a word taken at 199 us is fine, at 201 us MISS and the buffer holds the next word; write: a flag answered at 199 us is fine, at 201 us MISS at the interchange where that word was due, the writers go off before the stale word is written, and that space and every later one keep what the tape held (the block fails); the flags keep coming and GO stays set; search: an untaken search flag gives MISS at the next block; MISS never stops the tape |
| END | forward END entering the end zone, to the nanosecond, GO cleared, stopped 375 slots in; turning around in the end zone is not an error; reverse END from the load position on reaching speed, and from the block area after block 0; a tape selected while already inside the end zone ahead meets the end mark when the selection delay ends |
| tape unable | no unit, no tape, locked write (nothing starts); locked read works; a refused `mlc` leaves the motion and mode alone; `mse` clears UNABLE |
| off the reel | a deselected tape does not stop at the end zone, runs off the reel with no flags or END, is UNABLE until remounted, and a remount recovers it |
| write entry | write from rest raises DF on reaching speed; read -> write after word 50 puts the first word in the third space after the last read (two spaces lost); the manual's two exceptions (first word into slot 259 raises BEF; into slot 260 raises nothing until the next block's slot 2) |
| write enable | `mlc` go write sets WRITE ENABLE; a missed word clears it; `mse` clears MISS and ERF but writing stays off, and the flags answered in time after it write nothing; the next `mlc` go write turns it on and its word lands in the next space; `mlc` search clears it; END during a write clears it and GO, and the last block, written whole before the end zone, still checks |
| D256 latch | `mlc` search just after the checksum's `mwr` is held (mode and GO read as before), the checksum is written and block 131's flag comes 1.2 ms after the BEF; a stop at the last data word's flag is held, the BEF still comes, and the tape starts to stop only as the checksum is written; control leg: search at data word 255's flag takes effect at once and the block's end is not written; two commands in the window: the later one is carried out; `mse` in the window carries the held command out at once and the old drive's checksum is not written |
| All Halt | `mt550AllHalt()`, as the plugin calls it when RUN falls: the selected drive writing and a deselected drive at speed both stop, GO reads 0, the mode, REV and the errors are kept, writing is off, the partial block is flushed; stopped in 1 s with no flags after the halt; the block holds its words up to the halt and nothing after, the next block is untouched; a minute later neither has moved and the deselected one has not run off its reel; an `mlc` go moves the tape again; an accelerating drive and one part way through a turnaround simply stop, forward; an `mlc` held for a checksum is dropped and the checksum is not written |
| image files | mount from a file, write-through at the end of the block, remount keeps the data, locked mount refuses write; the same file recognized on a second path; a missing file with a locked mount is refused and not created; a part-block file, a block plus one word, an oversize file and a directory are refused; a read-only file mounts locked (skipped as root) |
| deferred formatting | a missing file is created empty and reads as a blank tape; writing block 5 grows the file to exactly 6 blocks with blank blocks 0-4 (no holes); rewriting block 2 does not grow it; a short file remounts with its blocks and a blank rest; mode 7 truncates the file to 0; writing the last block makes a full-size file; an in-memory tape erases; a locked tape refuses the erase |

The deadline windows the model gives. With the block mark in space 0,
where DEC's figures put it, every one equals DEC's figure; `-v` prints
the three that used to differ.

| After | DEC | Model |
|---|---|---|
| read BEF, switch to search | 800 us | 800 us |
| write BEF, switch to search | 1.2 ms | 1.2 ms |
| search flag, switch to read | 600 us | 600 us, to catch the leading checksum |
| search flag, switch to write | 400 us (`mlc`), 600 us (first `mwr`) | 400 us and 600 us |
| read BEF, switch to write | 1.2 ms | 1.2 ms |
| read BEF, next data flag | 1.4 ms | 1.4 ms |
| write BEF, next data flag | 1.6 ms | 1.6 ms |
| any data flag, answered | 200 us | 200 us |

Until 13-Sep-2026 the mark sat in space 1, so both search windows after
a BEF were one space (200 us) more generous than DEC's, and the leading
checksum could be caught only 400 us after the search flag.

## What plugintest covers

It stands in for the emulator: a `PDP1` struct, the configuration (a
`microtapesbs` setting), and `dynamicIotProcessBreak()`, which counts
breaks per channel. Each IOT is issued as `dynamicIotProcessor()` does,
TP7 (pulse 0) then TP10 (pulse 1), with the completion value computed
from bits 5 and 6.

| Group | Checks |
|---|---|
| microtapes.txt | comments, blank and all-blank lines skipped; a relative path created as an empty blank tape; an absolute path with `,locked` and trailing spaces mounted locked; a missing locked image neither mounted nor created; a path with a space; drive 9 rejected; a drive listed twice gets the later line and the earlier file is never made; a malformed line ignored; a path in a missing directory fails |
| device 01 routing | `rpa` (sub-device 0), sub-devices 1 and 010 reach the reader; `mse` (sub-device 3) reaches the tape and leaves the reader alone; read-in's `rpb` on device 2 with a dio word in MB whose bits 7-11 are 3 reaches the reader, with the control leg: the same MB on device 1 selects unit 5; the C bit gives the completion and a plain IOT does not; `mrs` after an accepted and a refused `mlc` |
| mount IOT 201 | a relative name mounts and creates the file; AC masked to 16 bits; an absolute name; the file already on another drive refused, that drive left empty; AC = 0 and AC = 200000 unmount; drive 0 and drive 011 fail and change nothing; a name running past the end of its bank refused, with the control leg: a name ending in the bank's last word mounts; an empty name, a control character and a 300-character name refused; a 5-byte file refused and left as it was; a read-only file gives IO = 1, mounted locked (skipped as root); a program's mount replaces a listed tape |
| SIGHUP | an unchanged list leaves a program's mounts alone, on a listed drive and an unlisted one; a failed line is retried and now mounts; a failed line on a drive a program has since used is not retried; a changed line remounts; removed lines unmount; a tape off its reel is rethreaded from the same file; a search flag breaks on the `microtapesbs` channel, 5 and then 7, and on no other |
| All Halt | through the I/O poll: RUN held at 0 (as at load time) stops nothing; RUN rising stops nothing; RUN falling stops the selected drive and a deselected one, and `mrs` shows GO clear; RUN rising again moves neither |
| mse reread | `mse` with the list unchanged leaves a program's mount and the listed drives alone, and still selects; an edited line takes effect at the next `mse`, on that drive only; the same bytes rewritten do not rewind a drive that has moved off the load point; two drives trade tapes in one edit; a list renamed into place (as `Tools/TapeUtils/mtp` writes it) is read; a bad line is reported once over three `mse`, not three times; a removed line unmounts its drive; a removed list unmounts every listed drive but not a program's; a list that reappears is read. Control legs (run on copies, 13-Sep-2026): with no reread at `mse`, 8 of these checks fail; with the list applied at every `mse` whether or not it changed, the reported-once check fails |

## What roundtrip.sh covers

`mkmicrotape`: a blank image is an empty file and checks as 576 blocks,
none in the file; `-f` is needed to overwrite; a blank image exports as
578 zero blocks; a full simh import (with the blocks 576-577 warning) is
full size and checks; export gives back the masked words; the round trip
is identical; a short simh file imports as only its blocks, and exports
as those blocks then zeros; `check` finds exactly the one damaged block;
a part-block file, a block plus two bytes and an oversize file are
refused.

## Emulator probes (Phase 5)

These need the emulator with the reader plugin built from these sources
installed, and the emulator to themselves: run them **one at a time, and
only when no other task holds the emulator**. Each program halts; the
result is in AC and IO. `make probes` assembles them with `am1 -S` (set
`AM1` and `AM1INC` if am1 or the include tree are not the defaults, e.g.
`make probes AM1=../../../bin/am1`, as the other tasks in this sandbox
use).

### The reader, first

The reader plugin now carries the tape, so check that it behaves as
before with a paper tape through the harness: `rpa`, `rpb`, and read-in
(whose `rpb` pulses carry a dio word in MB). This is required, not
optional: it is the one path where a mistake in the device-01 routing
would show.

### Tape probes

Configuration for the tape probes, in `/opt/pidp1-mods/microtapes.txt`:

    1 <blank image>
    2 <blank image>,locked
    (no line for drives 3 and 4)

A missing unlocked image is created blank; for drive 2's, make an empty
file first (or `../mkmicrotape blank <file>`), since a locked line does
not create one. Give each probe a fresh mount (restart, or change the
lines: the probe's first `mse` applies them). `probe_end` and
`probe_halt` must start with drive 1 at the load point.

Under the test harness, `/opt/pidp1-mods` is `run_in_opt.sh`'s per-run
directory of links to the checkout, so a file the plugin creates at its
top level lands there and goes when the run ends: `probe_mount`'s
`probemnt.img` does. Keep the listed images in a subdirectory of the
checkout (the 11-Sep runs used `1 mtimg/mt1.img` and
`2 mtimg/mt2.img,locked`) to find them afterwards.

| Program | Pass | Control leg |
|---|---|---|
| `mtsample.am1` | the example in Docs/UsingType550Microtape.md. It writes block 12 (octal; block 10 decimal) of drive 1 forward, reads it back in reverse, and halts at `good` with AC = 0. Afterwards, `mkmicrotape check` on drive 1's image finds no bad block, and the image is 11 blocks long (blocks 0-12 octal). | Its own total check: on a word lost at the search-to-read deadline it halts at `bad` with the total in AC. |
| `probe_period.am1` | halts at `done` with AC = the 10-block search time and IO = the 5-block time, in ms: about 1020 and 410 octal (528 and 264 ms). | The 5-block time must be half the 10-block time. |
| `probe_miss.am1` | halts at `done` with AC = 0 (the fast block: no error) and IO = 120000 (the slow block: ERF and MISS). | The fast block is the control. |
| `probe_end.am1` | halts at `pass`, AC = 0: moving in reverse from the load point gives END, ERF and GO cleared, and the tape stops. | Leg 1, moving forward, must show GO and no error. |
| `probe_unable.am1` | halts at `pass`, AC = 0: drive 3 with no tape, a write on locked drive 2, and no drive selected are refused (ERF and UNABLE); `mse` clears them. | Checks 3 and 4: a read on the locked drive and a write on drive 1 are accepted. |
| `probe_mount.am1` | halts at `pass`, AC = 0: `mmt` mounts `probemnt.img` on drive 4 (IO = 0) and the tape searches to block 0; mode 7 on drive 4 is accepted; `mmt` with AC = 0 unmounts drive 4 and a move on it is then refused; `mmt` with drive 0 and with an empty name give IO = 777776. Delete `/opt/pidp1-mods/probemnt.img` afterwards. | Check 4: mode 7 on the locked drive 2 is refused. |
| `probe_halt.am1` | needs a harness (see its header). It writes block 20 (octal) of drive 1 with the words 1, 2, ... and halts after 100 of them with the tape still writing; the harness waits 3 s and continues it. It then halts at `pass`, AC = 0: GO was clear after the halt (check 1), block 20 fails its check (check 2), and blocks 21 and 22 read as blank blocks (check 3). The image is 17 blocks (octal 0-20) and holds nothing past the halt. `hkind` picks the halt: 0 the `hlt` at `pause`, 1 a breakpoint at `brk`, 2 an ad1 `stop` while it spins at `spin`, restarted at `after`. `estep` is the single-step leg. | The plugin from before All Halt: `fail` with AC = 5 (checks 1 and 3), GO still set, and the image grown past block 20 with the last word written through the halt. |

### What has been checked without the emulator

10-Sep-2026: every program above then present assembled with `am1 -S`.
The tape probes and a loader probe were also run on a scratch PDP-1
instruction simulator, written from the handbook's instruction
descriptions at 5 us per memory cycle, whose IOTs went through a copy of
the real `dynamicIots.c` to the plugin as it was then built. Every probe
gave its pass result. `mtsample` still passed with every instruction made
2.5 times slower; at 2.8 times it lost the first word and halted at
`bad`, and at 3 times and beyond it halted with MISS.

11-Sep-2026: the tape moved into the reader plugin (Magtape/TASK-REWORK.md);
the loader probe and its test plugins went with the loader change.
Every probe, `probe_mount` included, assembles with `make probes`
(`bin/am1 -S`), and `plugintest` exercises the plugin's routing and
mounting on the host.
None of this replaces running the probes on the emulator.

### Emulator results, 11-Sep-2026

Run through the harness on a private WSL copy of the sandbox, with `pdp1`
(the `dynamicIots.c` alias fix in it) and every plugin rebuilt there by
`IOTs/install.sh`, one test per emulator start. All passed.

| Test | Result |
|---|---|
| reader, from a program | two `rpb` and two `rpa` read 123456, 654321, 101 and 042 from a punched tape, halting at `done` |
| read-in, raw tape | a tape of `dio 300`, `law 1234`, `dio 301`, `hlt`, `jmp 300`, whose dio words carry sub-device 3 in bits 7-11: PC 302, AC 1234, both words stored |
| read-in, am1 tape | an am1 `.rim` through its own loader: PC just past `done`, AC 3720 |
| reader control leg | the same three with the pre-rework `IOT_1.so` and `IOT_2.so` swapped in: identical results |
| `mtsample` | `good`, AC 0; the image is 11 blocks and `mkmicrotape check` finds no bad block |
| `probe_period` | `done`, AC 1020, IO 410 (528 and 264 ms, exactly two to one) |
| `probe_miss` | `done`, AC 0, IO 120000 |
| `probe_end` | `pass`, AC 0; the status left in IO, 150000, is ERF, END and REV with GO clear |
| `probe_unable` | `pass`, AC 0 |
| `probe_mount` | `pass`, AC 0; the plugin reported creating `probemnt.img` and refusing the empty name |

### Emulator results, 13-Sep-2026

`MiscTasks/Completed/TASK-TAPE-HALT-WRITE.md`, with the owner's `mse` reread.
Run through the harness in WSL, from a private runtime root under `~`
whose `IOTs/IOT_2.so` was this tree's build (the sandbox's `pdp1`, every
other plugin as installed), one emulator start per test. The control
legs swapped in the plugin installed before the task.

| Test | New plugin | Control leg (old plugin) |
|---|---|---|
| `mtsample` | `good`, AC 0; the image is 11 blocks and `mkmicrotape check` finds no bad block | |
| `probe_period` | `done`, AC 1020, IO 410 | |
| `probe_miss` | `done`, AC 0, IO 120000 | |
| `probe_end` | `pass`, AC 0; IO 150000 | |
| `probe_unable` | `pass`, AC 0 | |
| `probe_mount` | `pass`, AC 0 | |
| `probe_halt`, `hlt` | stopped at `pause`; then `pass`, AC 0, status after the halt 000000; block 20 holds words 1-99 and then zeros; image 17 blocks | `fail`, AC 5, status 724000 (GO set); the image grew to 106 blocks, the last word written through the halt |
| `probe_halt`, breakpoint | stopped at `brk` (PC `brk`+1, twice, 1 s apart); `pass`, AC 0; image 17 blocks | `fail`, AC 5, GO set; 119 blocks |
| `probe_halt`, ad1 `stop` | stopped at `spin`; `pass`, AC 0; status 720000 (DF, BEF, ERF, MISS; GO clear); block 20 holds words 1-100; image 17 blocks | `fail`, AC 5, GO set; 102 blocks |
| `mse` reread (live) | drive 1 changed with `Tools/TapeUtils/mtp` while the emulator ran, no SIGHUP: at the next `mse`, a locked line for a missing image left it UNABLE (status 101000), and a new image was created and mounted (GO, able); mounting the same line again left it able | the edits had no effect: GO, able, the new image never created |
| ZMachine Task 28, leg WS | ad1 `stop` during a save, the CPU halted 3 s: GO clear when the program looked; only the block being written is partial; the victim save 30 blocks later still totals zero block by block and restores whole; the image did not grow | the victim's 30 blocks all fail; the image grew to 136 blocks |

The stop switch itself was not pressed; ad1 `stop` drives the same
switch line. **Single-step does not stop the tape.** A stepped `mlc`
started the tape, and `mrs` stepped a second later still showed GO. A
debug build showed why: over six steps the I/O poll, which looks for RUN
falling, saw RUN up only once, for about 5 us of simulated time. Most
steps begin and end between two polls. This is left as a known
difference from the real control, whose relays dropped at every step.

The reader's own test (a paper tape through `rpa`, `rpb` and read-in) was
not rerun: `IOT_2.c` did not change.
