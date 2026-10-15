# Tests/Overlays -- drum overlays

`run_tests.sh` checks what am1 does with drum overlays: the six directives
(`%%drumreserve`, `%%window`, `%%overlay`, `%%endoverlay`, `%%ovcall`,
`%%ovtable`), the drum layout, the table and headers, the `.ovl`, `.osym` and
`.sym` files, and the overlay manager's include files. How overlays are
written and used is `Docs/UsingOverlays.md`.

Run it with the tree's own am1, which is its default (build it with `make` in
the am1 directory):

```
./run_tests.sh
./run_tests.sh /path/to/an/am1
./run_tests.sh -i /path/to/Am1Includes f1_valid f7_manager
```

A clean run reports **218** pass and **0** fail. The include root defaults to
the checkout's `Am1Includes`, four levels up, so that `DRUM/overlay.ah` and
`DRUM/overlay.ac` come from the tree under test and never from an installed
copy; `-i` names another. Fixture names after the options run only their
lines. Everything is assembled in a temporary directory that is removed at
the end; nothing is written here. It needs bash, awk, sed and grep, and no
emulator.

## How a check works

`MANIFEST` holds one check per line, `name|expect|flags|text`. Each assembles
`name.am1` with `-T` and the flags; a first flag of `bin` is dropped and `-T`
left off, so that the binary generator runs, since some checks (a root word
in a window, for one) are made only there. `expect` says what must hold:

| expect | holds when |
|---|---|
| `ok` | am1 exits 0 |
| `okgrep`, `lstgrep`, `outgrep` | it exits 0, and the `.dmp`, `.lst` or stdout has a line matching the regex `text` |
| `optgrep`, `ovlgrep` | it exits 0, and the optimizer's report (the `.opt`) or the `.ovl` has a line matching the regex `text` |
| `nooptgrep` | it exits 0, a `.opt` is written, and no line of it matches the regex `text` |
| `same` | it exits 0, and the file `name.text` it writes equals `name.text.ref`, this directory's path taken out |
| `hand`, `nohand` | it exits 0, and the file `name.text` it writes equals (or, for `nohand`, differs from) the one `hand/name.am1` writes, assembled with the same flags but no `-O`; `hand/name.am1` is the program with the optimizer's rewrites made by hand |
| `refsub` | it exits 0, and every line of `name.ref` is a line of the `.dmp` |
| `fail` | it exits non-zero, with `text` in stderr, so a fixture that fails for another reason fails its check |
| `warn`, `nowarn` | it exits 0, and stderr does, or does not, hold `text` |
| `pairs`, `nopairs` | it exits 0, and `N` words from `A` agree (or, for `nopairs`, do not all agree) with the `N` from `B`; `text` is `A B N` in octal, and `A/name` means overlay `name`'s words at `A` |

## The fixtures

Every fixture and `MANIFEST` are written by `make_fixtures.py`
(`python3 -I make_fixtures.py`, here). They are regenerated, never edited:
change the base program or a variant in the generator and run it again. The
words the `.ref` files hold, the stamp among them, are worked out by hand in
the generator, never copied from am1's output.

| Fixtures | What they hold |
|---|---|
| `f1_valid` and its 37 variants, `f8_varro` | the directives: `f1_valid` is the control, two windows and three overlays, and must assemble with the words its lines name; each variant makes one change and must fail, or warn, with the one message its line names |
| `f2_valid` and its 13 variants | `%%ovcall`, references between the root and the overlays, and a root word in a window, in the same way |
| `f3_ovcall` | the words `%%ovcall` assembles, against `f3_ovcall.ref` |
| `f4_layout`, `f4_fit`, `f4_full` | the drum layout `-S` prints: tracks, windows and banks; 32 overlays fit and a 33rd does not |
| `f5_table`, `f5_word`, `f5_track` | the table, the headers and the stamp; the two variants must change the stamp |
| `f6_files` | the `.ovl`, `.osym` and `.sym` files, against their `.ref` files |
| `f7_defines`, `f7_swapped`, `f7_manager` | `DRUM/overlay.ah`'s offsets agree with the table am1 builds (`f7_swapped` is the control that must disagree), and `DRUM/overlay.ac` assembles in a program |
| `f9_backward`, `f9_ascending`, `f9_table`, `f9_auto`, `f9_bank1` | `-S`'s figure per bank is the highest word, assembled or reserved by a *table*, wherever the counter ended: an origin back to lower addresses, an empty origin at the end, constants and variables am1 places itself, a bank with no word (`f9_ascending` is the control, the same words in ascending order) |
| `f10_pins` and its 4 variants, `f10_window`, `f10_windowhigh` | the optimizer's root view: the root rewritten and moved at `-O1`, a root word an overlay names held in place, an overlay word naming a root word that moves following it, and a window kept clear of the root's growth |
| `f11_twins` and its 3 variants, `f11_pins`, `f11_nopin` | each overlay rewritten in a view of its own: `f11_twins`'s two overlays of one window each take their own rewrite, worked out by hand, though each writes the word the other's rewrite reads; its variants open a region at an overlay's boundary, an error; in `f11_pins` another overlay's patch of a root word refuses a jump through it (`f11_nopin`, without the patch, is the control) |
| `f11_shrink`, `f11_full`, `f11_roomy` | relayout in an overlay's view: `f11_shrink`'s deletion moves only the overlay's words, its `%%ovcall` entry word, table and stamp with them, its files equal to its hand build's; `f11_full`'s unroll does not fit its window and is refused for the budget, and `f11_roomy`'s, with exactly the room, is taken |

## What it does not cover

- **Running an overlay program.** Loading the drum (`ovlload`) and the
  manager swapping overlays on the emulator are tested where those were
  built, not here: this suite needs am1 alone.
- **The optimizer's other tests.** F10 and F11 hold the rows that need am1
  alone. The tests comparing whole builds with another am1, and those
  needing `am1test`, are not part of this suite.
