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
deadlines).

| Group | Checks |
|---|---|
| 5.2 IOTs | power-on status; `mlc` refused with no unit or no tape (UNABLE + ERF, one break); unit numbers 1-7, 010 = 8, 0/011-017 = none, IO bits 0-1 and 6-17 ignored; `mwr`/`mrd` move the 18-bit buffer and clear only DF/BEF; `mse` clears ERF and UNABLE; modes 4-6 run the tape with no flags and leave it untouched; mode 7 blanks the reel (a written block reads blank and every block checks), raises no flags and runs the tape; mode 7 on a locked unit is refused and the tape is intact; REV and GO status bits; MTE never set |
| 5.3 search and delays | first search flag from the load position to the nanosecond; one flag per block, 52.8 ms apart; search words `260000\|block` and `450000\|block`; start delay (block marks passed during the 200 ms ramp raise nothing); turnaround delay (300 ms); selection delay (34 ms, with a control leg that reselects early enough); reselecting the same unit starts no delay; 150 ms stop over 375 slots |
| 5.3 read | forward: DF at the end of slots 3-259, BEF at 260, 200 us apart, one break per flag, words as stored; reverse: the same block last word first (decision D3); both total -0 |
| 5.3 write | entering write raises DF at once; DFs at the ends of slots 3-257, BEF at 258; -0, the data and the checksum land in slots 3, 4-259 and 260; block totals -0; neighbors untouched; one write-through flush; reverse write mirrors the slots; forward write / reverse read and reverse write / forward read identities; a partial rewrite leaves the block failing its checksum |
| 3.5 block-end deadlines | read BEF -> next DF at 1.4 ms (hit at 1.3 ms, MISS at 1.4 ms with the buffer overwritten); write BEF -> next DF at 1.6 ms (hit, and MISS with the stale buffer written as the checksum); read BEF -> search (800 us and 999 us hit, 1001 us misses a block); write BEF -> search (1200 us and 1399 us hit, 1401 us misses); read BEF -> write (1.15 ms writes the next block correctly, 1.25 ms does not) |
| 3.5 search-flag deadlines | search -> write at 150 and 380 us writes the block correctly, at 410 us word 1 slips a slot; search -> read at 390 us catches the leading checksum, at 410 and 590 us data word 1, at 610 us only word 2 |
| 3.5 word deadline and MISS | read: a word taken at 199 us is fine, at 201 us MISS and the buffer holds the next word; write: a flag answered at 199 us is fine, at 201 us MISS and the stale word is written twice; search: an untaken search flag gives MISS at the next block; MISS never stops the tape |
| END | forward END entering the end zone, to the nanosecond, GO cleared, stopped 375 slots in; turning around in the end zone is not an error; reverse END from the load position on reaching speed, and from the block area after block 0; a tape selected while already inside the end zone ahead meets the end mark when the selection delay ends |
| tape unable | no unit, no tape, locked write (nothing starts); locked read works; a refused `mlc` leaves the motion and mode alone; `mse` clears UNABLE |
| off the reel | a deselected tape does not stop at the end zone, runs off the reel with no flags or END, is UNABLE until remounted, and a remount recovers it |
| write entry | write from rest raises DF on reaching speed; read -> write after word 50 puts the first word in the third space after the last read (two spaces lost); the manual's two exceptions (first word into slot 259 raises BEF; into slot 260 raises nothing until the next block's slot 2) |
| image files | mount from a file, write-through at the end of the block, remount keeps the data, locked mount refuses write; the same file recognized on a second path; a missing file with a locked mount is refused and not created; a part-block file, a block plus one word, an oversize file and a directory are refused; a read-only file mounts locked (skipped as root) |
| deferred formatting | a missing file is created empty and reads as a blank tape; writing block 5 grows the file to exactly 6 blocks with blank blocks 0-4 (no holes); rewriting block 2 does not grow it; a short file remounts with its blocks and a blank rest; mode 7 truncates the file to 0; writing the last block makes a full-size file; an in-memory tape erases; a locked tape refuses the erase |

The deadline windows the model gives, printed with `-v`:

| After | Manual | Model |
|---|---|---|
| read BEF, switch to search | 800 us | 1000 us (lenient by one slot) |
| write BEF, switch to search | 1.2 ms | 1.4 ms (lenient by one slot) |
| search flag, switch to read | 600 us | 400 us to catch the leading checksum, 600 us to catch data word 1 |
| search flag, switch to write | 400 us | 400 us (the first word must be in the buffer by then too) |
| read BEF, switch to write | 1.2 ms | 1.2 ms |

The search-to-read figure agrees with the manual once you count data
word 1. On a block written forward, the leading checksum is the automatic
-0, which leaves a ones'-complement sum unchanged. So a program that
switches between 400 and 600 us still checks the block.

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
lines and reload). `probe_end` must start with drive 1 at the load point.

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
