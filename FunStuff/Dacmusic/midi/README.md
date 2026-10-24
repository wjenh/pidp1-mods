# midi2dac, a MIDI file to a tape for the D/A converters

`midi2dac` turns one or more Standard MIDI Files into a paper tape image
for the D/A converters' music player, which reads it with READ TAPE on
its menu. The tape holds Peter Samson's Harmony Compiler parts in the
1968 tape format, with the player's dynamics and, if you ask, an
orchestra block that sets each converter's part, voices, octave, volume
and place, and the room and the onset. No PDP-1 and no Harmony Compiler
are needed. The tape plays on the player, renders with
`../render/dacrender`, and punches from the player's PUNCH like any
other.

The player has four converters, so a piece gets at most four parts, one
note at a time each. A MIDI file seldom fits that. A piano plays chords,
and an organ score holds notes under moving ones. `midi2dac` shares the
four parts among the file's tracks, splits each track into its parts,
and reports every note it had to cut short or leave out, by part and
bar. It writes each note in the words the Harmony Compiler compiles it
to, so its tape is the one the compiler would punch from the same music.

This directory has a second program, `midi2hc`, which turns a MIDI file
into source text for the Harmony Compiler instead, to read, edit and
compile. It is described at the end of this file.

## Build

Any C99 compiler and its C library, nothing else. `make` in `..` builds
it with the rest. To build it alone, from this directory:

```
make
```

or by hand:

```
cc -O2 -std=c99 -ffp-contract=off -o midi2dac midi2dac.c
```

Keep `-ffp-contract=off`. It makes every host write the same tape. It
builds on Linux (a Raspberry Pi included), macOS, and Windows under WSL
or MinGW.

## Run

```
midi2dac [options] MIDI... -o TAPE.bin
```

From `/opt/pidp1-mods/FunStuff/Dacmusic`, with a MIDI file:

```
midi/midi2dac ~/amazing-grace.mid -o ~/grace.bin
```

```
amazing-grace.mid: group 1 (Piano): the sustain pedal passed over
amazing-grace.mid: group 1 (Piano): sounds 5 at once, into 4 parts: 0 cut, 22 dropped
grace.bin: 4 parts, 1 movement of 4, 376 words: below TEND, 45056 words for the voices
```

Then load the player, READ TAPE, set PARTS A MOVEMENT to the summary's
"of 4", set the orchestra, and PLAY (`../UsingDAC.md`, "Playing a
tape"). A tape with an orchestra block (`--orchestra`, below) sets all
of that itself.

| Option | What it does |
|---|---|
| `-o TAPE.bin` | The tape image to write. Needed |
| `--played` | The file was played in, not engraved. Every note is snapped to 64ths, at the offset that moves them least, and the report says how far they moved. Triplets are lost |
| `--tempo N` | The first tempo word is N (1 to 32767), and the file's other tempo changes follow in proportion, as the Harmony Compiler's TEMPO scales them (`/opt/pidp1-mods/Docs/UsingHarmony.md`). The word plays 12,174 / N quarter notes a minute |
| `--transpose N` | Move every note N semitones, up or down |
| `--by-channel` | Each channel of a track is a group (below, "Parts") |
| `--parts SPEC` | Which groups make which parts, and how many each gets (below) |
| `--organ` | Legato from a half note up, normal below, as an organ is played. Without it, each note's articulation comes from the silence before the next |
| `--dynamics=velocity` | The dynamics from the notes' velocities. The default |
| `--dynamics=cc7`, `--dynamics=cc11` | The dynamics from the volume (CC 7) or expression (CC 11) controller |
| `--dynamics=none` | No dynamics, so the tape plays at the sliders |
| `--orchestra FILE` | An orchestra block from FILE (below) |
| `--orchestra=gm` | An orchestra block from the file's General MIDI programs, pans and volumes |
| `--no-share` | Every bar written out (below, "Bars stored once") |
| `--plain` | A tape for `pdp1m13` and the 1.0 player, with no dynamics and no orchestra block |
| `-q` | The summary only, not the reports |

The exit status is 0 with a tape written, 1 when the file is refused
(nothing is written), and 2 for a command line it cannot take.

## Several files make movements

Each file on the command line is a movement, played in order with two
seconds between them, as Samson's trio sonatas play:

```
midi/midi2dac first.mid second.mid third.mid -o sonata.bin
```

Every movement has the same number of parts, PARTS A MOVEMENT, the most
any file has. A file with fewer gets parts of rests, and says so, as in
`second.mid: 1 part of rests added, for 4 parts a movement`. A tape
holds at most 30 parts, so 7 movements of 4, or 10 of 3.

## Parts

A group is a track of a format 1 file, every channel of it together, or
a channel of a format 0 file. `--by-channel` makes each channel of a
track a group. A group that is only channel 10, General MIDI's
percussion, is left out and reported, since the player has no drums.

Each group gets a part, and the parts left of the four go one at a time
to the group sounding the most notes at once beyond its share. More than
four groups is refused, naming them, and `--parts` then chooses.

A group sounding more at once than its parts is split. At each new chord
the highest and lowest notes are kept, then new notes before held ones,
then a pitch class not yet among those kept, then the note nearest the
middle. The notes go to the parts by the smallest leaps. A held note
left out is cut where the new one starts, a new one left out is dropped,
and the report gives the counts, as in `sounds 5 at once, into 4 parts:
0 cut, 22 dropped`. A group in one part keeps the highest note of each
chord. Two notes at one key starting together (two tracks merged playing
the same note) are one note, the longer, and counted as doubled.

Overlaps of a 32nd or less, a played legato, are cut at the next onset in
every file, and reported by group.

`--parts` names the groups, by their numbers in the reports, and the
parts each gets. It is a list of entries separated by commas, each one
or more groups joined by `+`, then optionally `:N` for its parts (1 if
not given), at most four parts in all. Groups not named are left out and
reported. The Toccata's two hands as three parts and its pedal as one,
the arrangement of `../tapes/toccata-biggs.bin`:

```
midi/midi2dac ToccataFugue.mid --parts 1+2:3,3 -o toccata.bin
```

## The notes

The player's unit is 1/192 of a whole note, so a 64th is 3 and a triplet
64th 2. Any division of the quarter note is taken. A note is on the grid
when it is within one tick of a unit, so a file that rounds its triplets
to ticks is still exact. A note off the grid is refused with its group
and bar, unless `--played`.

A note's length and articulation come from the silence before the next
note in its part, as in the Harmony Compiler. The articulations are
legato, normal, quarter, half and staccato, the note silent for none of
its time, an eighth, a quarter, a half or five eighths, and the nearest
is taken. When the silence is longer, the note keeps its end and a rest
follows. A note too long for one word, or across a bar line, becomes
legato notes at one pitch. `--organ` instead plays legato from a half
note up and normal below.

The keys are MIDI 24 to 85 (C1 to C#6), or 24 to 84 with `--plain`, as
in `pdp1m13`'s table. A key outside is refused, with its group and bar.
`--transpose` moves the file.

The bars come from the file's meters, and a meter that is not a whole
number of 32nds is refused. Every part ends at the last bar line of the
longest.

## Tempo

Each tempo change in the file is a tempo word, in part 1, at the note
starting there. Where part 1 has none, a held note or a rest, the note
or rest is cut in two there, which sounds the same. The player takes at
most 16 tempo words in a bar. A bar with more keeps the first 15 and the
last, and says so.

## Dynamics

The player plays six levels, pp, p, mp, mf, f and ff, each converter at
its slider's volume for f. From velocity, each note is given the nearest
level (velocities 33, 49, 64, 80, 96 and 112) and a dynamic is written
when the level moves two or more, or stays one away for three notes, so
a performance's small changes are not each a dynamic. A part whose notes
all have one velocity, a file from a notation program, gets none and
plays at the sliders. Each movement starts at f. The player queues at
most four dynamics a bar in a part, and a bar with more keeps the last
four and says so. With `cc7` or `cc11`, each note takes the controller's
last value at or before it in place of its velocity (f before the
first), so a change applies from the next note on.

## Bars stored once

A bar that is the same as one before it in its part, its notes and
dynamics, is stored once and pointed at again, as the tape format
allows. It saves 4% to 42% of the words, the most on folk tunes and
the least on music played in. `--no-share` writes every bar out.

## Size

The player keeps a tape in core from 020000 to TEND, 036400, 7,424
words, and a larger one runs on from 050000, with the voices after it.
The summary says which, and how many words are left for the voices. A
tape too large for both is refused. A plain tape is refused over the 1.0
player's room, 7,872 words counted as each part's notes, twice its bar
pointers and six words, then four more.

## The orchestra file

`--orchestra FILE` writes an orchestra block, so the tape plays its
voices, octaves, volumes and places whatever the menu was set to. The
file is text, a setting a line. A `#` starts a comment, and case does
not count:

```
# Scotland the Brave on the bagpipe: the chanter the tune, the drones each side.
library /opt/pidp1-mods/FunStuff/Dacmusic/voices/lib/bagpipe.lib
room voices
onset off
1 part 1 bagpipe-chanter center
2 part 2 bagpipe-drones l1 -6dB
3 part 3 bagpipe-drones r1 -6dB
```

- `library FILE` names the voice library (`.lib`) whose voices the lines
  name, a full path or one relative to the orchestra file's directory.
  It must be the library the drum was made from, since the tape names
  its voices by their numbers in it. Without a `library` line the names
  are the nine basic voices, `flute`, `reed`, `plucked-bass`, `marimba`,
  `principal`, `gedackt`, `trumpet`, `bourdon` and `harpsichord`.
- `room` is `none`, `hall`, `busch`, `voices`, or `config` (the
  emulator's `dacroom`, the default).
- `onset` is `off`, `2`, `4`, `6`, `8`, `12` or `16` ms, or `config`
  (the emulator's `daconset`, the default).
- A converter's line is its number, 1 to 4, then `part N` (N from 1 to
  PARTS A MOVEMENT) or `none`, then, in any order:
  - one to four voices, by the name of the voice's file without
    `.voice` (`hw-prestant-8` for `flentrop/hw-prestant-8.voice`), or as
    the library writes the entry;
  - an octave, `8ft` (the default), `16ft`, `4ft`, `32ft` or `2ft`;
  - a volume, `0dB` (the default) to `-45dB` in 3 dB steps, the
    slider's levels;
  - a place, `left`, `l3`, `l2`, `l1`, `center`, `r1`, `r2`, `r3` or
    `right` (by default converters 1 and 2 left, 3 and 4 right).

A converter with no line plays no part. Several voices on one converter
sound together only as the player allows. Each must be an organ stop
(`mode stop` in its voice file) and all of one family, the Flentrop's
Hoofdwerk stops together for one, as `../UsingDAC.md`'s "Organ stops"
describes. Any other mix is refused, as the player's menu would refuse
it (STOPS DO NOT MATCH).

`--orchestra=gm` makes the block from the first file instead, part 1 on
converter 1 and so on. The General MIDI program chooses a voice of the
nine. Pianos, guitars and harpsichords take the harpsichord, chromatic
percussion the marimba, organs the principal, basses the plucked bass,
brass the trumpet, reeds the reed, pipes the flute, and any other the
principal, reported. The pan (CC 10) chooses the nearest of the nine
places, and the volume (CC 7) the nearest slider level. The block plays
on any drum with the nine. The nine are recorded on the standard and
Hammond drums (synthesized on the Flentrop's), the harpsichord 38,010
words, so a file mixing pianos, guitars or harpsichords with any voice
but the flute and the plucked bass is too large for core, and PLAY
refuses it (VOICES TOO LARGE; `../UsingDAC.md`, "The recorded
instruments").

With neither, the tape has no block. READ TAPE leaves the menu's
orchestra as it was, and you set it, with PARTS A MOVEMENT, on the menu.
PUNCH then writes a tape with it.

## Plain tapes

`--plain` makes a tape `pdp1m13` and the 1.0 player load, with no
dynamics, no orchestra block, keys 24 to 84, and no more than 7,872
words. `--plain` with `--orchestra` is a command line error.

## The reports

Before the summary, one line for each change the program made, by file
and group, or part and bar:

| Report | What happened |
|---|---|
| `group G: sounds N at once, into P parts: C cut, D dropped` | The group was split. C held notes ended early and D new notes were left out. A `, K doubled` after it counts notes at one key at once made one |
| `group G: N brief overlaps cut at the next onset` | A played legato |
| `snapped to 64ths at an offset of T ticks, notes moved up to M ticks` | `--played` |
| `bar B: N tempo words, the first 15 and the last kept` | More than the player takes in a bar |
| `part P, bar B: N dynamics, the last 4 kept` | More than the player queues in a bar |
| `group G left out (--parts)` | Not named in `--parts` |
| `group G: channel 10, percussion, left out` | |
| `group G: N pitch bends dropped` | The player plays equal temperament |
| `group G: the sustain pedal passed over` | One note a converter cannot hold a chord |
| `N unpaired note events passed over` | A note on with no note off, or the reverse |
| `part P: program N has no voice among the nine, the principal` | `--orchestra=gm` |
| `N parts of rests added, for P parts a movement` | A movement with fewer parts than the most |

The summary gives the tape, its parts and movements (`of P` is PARTS A
MOVEMENT), its words, and where it sits in core.

## The refusals

A refusal prints `midi2dac: refused:` and the reason, and nothing is
written. The refusals include a file that is not a Standard MIDI File,
format 2, SMPTE time; a note off the grid (`--played` snaps it); a key
outside the player's; a meter not of 32nds; a tempo the word cannot
hold, or a tempo change off the grid; more than four groups (`--parts`
chooses), more than four parts in `--parts`, more than 30 parts on the
tape; a tape too large; and in an orchestra file, a voice not in the
library, a voice file that cannot be read, more than four voices or
voices that cannot sound together on one converter, a volume not of the
slider's, a part the tape does not have, a converter given twice, and a
line not understood, each with the file and line.

## Not carried

The player plays one note at a time on each converter, in equal
temperament, with no pitch bend, so a sustain pedal's held chord, a
temperament, pitch bends, aftertouch and lyrics are dropped. A program
change after the first note, and a pan or volume change, plays only as
far as `--orchestra=gm` and the dynamics take them.

# midi2hc, a MIDI file as Harmony Compiler source

`midi2hc` turns a Standard MIDI File into source text in the language of
Samson's Harmony Compiler, `hc1d`, as a plain ASCII file. Harmony
(`../harmony`, `Docs/UsingHarmony.md`) reads the file from the paper
tape reader as it is and compiles it, for `pdp1m13` or the D/A
converters' player. The text is meant to be read and changed: every bar
is on a line of its own and numbered, so the compiler checks each bar's
count, and every accidental is written on its note.

It is built with `midi2dac`, by `make` here or in `..`, or by hand:

```
cc -O2 -std=c99 -ffp-contract=off -o midi2hc midi2hc.c
```

## Run

```
midi2hc [--played] [--title T] [--tempo N] MIDI OUT.txt
```

- `--played` takes a file played in by hand: its onsets and ends are
  snapped to a grid of 64ths, at the offset that fits them best.
  Without it, every note must start and end on the grid exactly.
- `--title T` heads each part with T; by default the MIDI file's name.
- `--tempo N` makes the first tempo word N, in `hc1d`'s numbers, and each
  later one in the same proportion, so the piece plays faster or slower
  with its tempo changes kept.

The summary gives the text's parts, bars and tempo words, each part's
notes by articulation (`l` legato, `e` normal, `q` quarter, `h` half, `s`
staccato), the longest run of characters each part puts in the
compiler's measure buffer, and with `--played` the snap: the offset and
how far the onsets moved. Then mount the text on the reader and start
Harmony (`Docs/UsingHarmony.md`, "Running it").

## The parts

Each track with notes is a part, or in a type 0 file each channel. A
part plays one note at a time, as the compiler's parts do, so two notes
at once in a part are refused with the part and bar. With `--played`
only two onsets together are refused; a note held into the next is cut
there.

## The notes

The unit is 1/192 of a whole note, so a 64th is 3 and a triplet 64th 2.
The file's division must be a multiple of 48 ticks a quarter, so that a
unit is a whole number of ticks. A note off the grid is refused with its
part and bar: shorter than a 64th, a quintuplet, anything the compiler
has no length for. A length no one note holds becomes notes at one
pitch, each legato but the last, a tie; triplets are written where a
length needs them.

A note's length and articulation come from the silence before the next
onset in its part, as for `midi2dac` above. The articulations are
legato, normal, quarter, half and staccato, the note silent for none of
its time, an eighth, a quarter, a half or five eighths. When the
silence, as a share of the time to the next onset, is within 1/16 of one
of those, the note runs to that onset with that articulation (with
`--played`, a silence of a 64th or less does too); otherwise the note
keeps its own end, with the articulation nearest its own silence, and a
rest follows. Where most of a bar's notes share an articulation other
than the mode in force, it is written once at the bar's start as the
new mode, which holds until changed, and only the notes that differ
(and a note before a rest) carry a letter.

The keys are MIDI 24 to 84, C1 to C6, the compiler's range; a key
outside is refused. Each part takes the clef whose middle line is
nearest its middle note, and notes below its staff are written with `b`.
There is no key signature: each sharp and flat is written on its note,
and a natural where the bar's earlier accidental would otherwise hold,
since the compiler holds one to the bar's end.

The bars come from the file's meters, written as `units` at bar 1 and
where the meter changes. A meter that is not a whole number of 32nds is
refused.

## Tempo

The file's tempo changes are `tempo` words in part 1, in `hc1d`'s
numbers, N for 12,174 / N quarter notes a minute, so a higher number is
slower. A change must fall on a note of part 1: on a rest it is refused,
since the compiler would mark the note before the rest, not the rest.

## The measure buffer

The compiler reads the source into a measure buffer of 384 characters a
fill. A fill ends at the first slash from about the 256th character on,
and one that reaches 384 stops the compiler with *Measure has too many
characters*. Where the next bar would carry a fill past 380, the bar
before it gets spaces before its slash, which the compiler passes over,
so the fill ends there and the long bar starts the next. A bar too long
for the buffer even then is refused.

## The check

Before it writes the text, `midi2hc` reads it back as the compiler reads
it, with a reader written from the language alone, and compares the
notes and tempo words with the ones it meant. A text that does not read
back is not written: it prints `the text does not read back:` and what
differs, which is a fault in `midi2hc`, or a title with a slash in it,
which ends the title early.

## The refusals

A refusal prints `refused:` and the reason, and nothing is written. The
refusals are a file that is not a Standard MIDI File or ends inside an
event, SMPTE time, a division not a multiple of 48, no notes, a meter
not of 32nds, a note off the grid, two notes at once, a key outside C1
to C6, a length no word holds, a tempo change off the grid or on a
rest, a tempo or `--tempo` that makes a word outside 1 to 32767, and a
bar too long for the measure buffer. A wrong command line prints the
usage and ends with status 2.

## Not carried

The compiler's parts have no dynamics, voices or controllers, so
velocities, program changes, controllers, pitch bends and the tracks'
names are dropped. For dynamics and an orchestra, `midi2dac` makes the
tape directly.
