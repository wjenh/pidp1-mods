# Voices for the D/A converters' music program

A voice is one instrument of the music program, as a file of its own:
its tables or samples, the zone of every pitch, its steps, envelope,
timbres and sample loop. The renderer (`../render/dacrender`) plays a
tape against a library of voice files; the drum writer, `dacdrum`, puts
a library on the Type 23 drum, from which the player
(`../music/dacmusic.am1`) loads the voices an orchestra names. How to
use them is `../UsingDAC.md`; how the device plays them is
`../Implementation.md`.

| file | what |
|---|---|
| `lib/*.voice`, `lib/standard.lib` | the nine voices of the music program, numbered 0-8 by `standard.lib` as every tape names them, all nine recorded; `lib/*-synth.voice`, the synthesized nine, are voices 0-8 of `flentrop.lib`, and `flute-synth.voice` voice 0 of the recorded instruments' libraries |
| `lib/flentrop/*.voice`, `lib/flentrop.lib` | the 27 stops of the 1958 Flentrop in Adolphus Busch Hall, a family a division; `flentrop.lib` numbers the synthesized nine 0-8 (the recorded harpsichord does not fit the drum beside the stops, and the Flentrop keeps its sound), and the stops 9-35 |
| `lib/bagpipe-*.voice`, `lib/bagpipe.lib` | the Highland bagpipe: `bagpipe.lib` numbers the chanter 0 and the drones 1; the chanter is recorded, the drones a table |
| `dacvoice.c`, `Makefile` | the generator: a table voice or a stop from a definition |
| `dacdrum.c` | the drum writer: a library to the drum file |
| `defs/*.def` | definitions: the five synthesized table voices (`lib/flute-synth.voice`, `reed-synth`, `plucked-bass-synth`, `marimba-synth`, `trumpet-synth`) and the bagpipe's drones (`dacvoice` remakes them word for word), `bagpipe-chanter.def`, a synthesized chanter no library uses, and `clarinet.def`, a new one |
| `defs/flentrop/*.def` | the Flentrop's stops, each division's lines (n, frame, envelope, chiff) the same in all its stops |

The recorded instruments and the Hammond's voices are in
`../instruments/`, in the same format.

## Voice files

Plain text, a field a line; a line that starts with a number continues
the field above it, and `#` starts a comment. Numbers are decimal but a
block's words, which are octal.

```
dacvoice 1                  the format's version
name flute
family winds                the menu's family, whose sublist shows the
                            voice (optional: a voice with none is in OTHER)
n 10                        the sample period, in machine cycles
mode table                  table or sample
envelope 240 0 874 10 48 874
                            the device's latch values: start and peak
                            (attenuation, 1/8 dB), attack and decay (rate
                            units), sustain (1/8 dB; 511: none), release
stride 256                  from one timbre's table to the next; 0 for one
thresholds 40 120 511       the followed timbre's levels, 1/8 dB; 511: none
sample 1600 320             a sample's end and loop, in words (a sample
                            voice only)
steps                       the step of every pitch 0-63 (0 and 1 are rests)
    0 0 1715 1817 ...
zones                       the block every pitch 0-63 plays
    0 0 0 0 ...
block 0 768                 a block's number (in order from 0) and length,
    056500 063400 ...       then its words
```

A table voice's block is its timbres' 256-word periods one after
another; a sample voice's is the attack, then the loop, to the end.
Pitches that play the same block name the same number, and the reader
places each block once: a file holds no address.

Version 2 adds the organ stop ("Stops" below), and a reader of 2 reads
1; a file that uses nothing of 2 is still written as 1, so the nine's
files are version 1:

```
dacvoice 2
mode stop                   a table of one timbre a zone, at a quarter of
                            full scale, and a speech block a zone
sample 1280 256             the end and loop of the voice the player's
                            premix makes: 1,024 words of speech, then the
                            256-word loop
speech                      each zone's speech block, zones 0-5 (an octave
    6 7 8 9 10 11           each from pitch 2), and zone 6's in a
                            version 5 stop with zone 6
```

Version 3 adds the voicing ("The voicing" below), and a reader of 3
reads 1 and 2; a file that uses none of it is still written as 1 or 2,
so the nine's files are unchanged:

```
dacvoice 3
edge -30                    the edge tone, dB under the stop at C4: even,
                            -34 to -22
rise 10                     the speech: the rise over 10 of the note's
                            periods (10 is the one value)
room busch                  the room the voice asks for: busch or hall
```

Version 4 adds the céleste, and a reader of 4 reads 1 to 3; a file
without it is still written as 1 to 3:

```
dacvoice 4
celeste 2                   the céleste: a second read, this many cents
                            sharp, 0.5 to 7.5 in half cents
```

Version 5 adds the converter for recorded instruments ("Recorded
instruments" below; `../Implementation.md`, "Recorded instruments"),
and a reader of 5 reads 1 to 4; a file without it is still written as
1 to 4. Each line is optional, each a latch value as `envelope`'s are:

```
dacvoice 5
interpolate                 dyx's action 040, held with the mode's
vibrato 29528 190 59        the slow counter's rate (dyv, 1 to 262143), the
                            vibrato's depth (0-511) and delay (0-255, in 16
                            clocks); or vibrato RATE DEPTH free, free running
leslie 24 48                the tremolo's depth and the place's (dym), 0-511
                            each, in 1/8 dB; it needs a vibrato line, the
                            counter's rate (its depth may be 0)
percussion 24 2 26 3        its level under the note (1/8 dB), the multiple
                            k (1-7), its decay rate (1 to 262143), and the
                            block holding its sine (256 words or more)
block 0 7200 2400 800 2400  a block a zone names may give its own end,
                            loop and stride after its length, in sample
                            mode only; one that gives none takes the
                            voice's sample and stride lines
pack                        dyx's action 0100, sample mode only: three
                            12-bit samples in each two words
zone6 1                     after the zones: zone 6's block, pitches 64-75
                            cut for C#7, which only a note an octave up
                            plays; a stop with it has seven speech
                            blocks, zone 6's last
```

A voice without a `zone6` line has no zone 6: its top zone in the map
is cut for B6 and plays its notes an octave up to B6. With one, the
map's top zone is cut for C#6 and zone 6 plays them to C#7. A file with
`zone6` is version 5; dacvoice writes it in every table voice and stop
but those whose definition says `zones 6` (below).

A block's every word a note can fetch, its end plus a stride for each
threshold, is in its length. A packed voice's end, loop and stride, the
`sample` and `stride` lines' and each block's own, are in groups of
three samples, two words each, so a block holds 2 x (end + stride x the
thresholds) words; a packed end is at most 21,845 groups. The
percussion's sine is not packed, so no zone of a packed voice names its
block. A block longer than 4,096 words, a drum
field, is taken: it runs on into the fields after it (`dacdrum` below).

A library, `.lib`, lists voice files, one a line, relative to the
library's own directory. A voice's number, the number an orchestra
block names, is its place in the list from 0; blank lines and comments
are not counted.

Put a small table voice first. The player loads voice 0 for a converter
the tape gives no part, so an idle converter holds voice 0 in core, and
the voices of an orchestra must fit banks 5-15 together, 45,056 words.
A large voice there (the recorded instruments are 13,000 to 38,000
words) would take room a part needs. `standard.lib` and the Hammond's
begin with the recorded flute, `flute.voice`, 5,408 words; the
Flentrop's and the recorded instruments' with the synthesized one,
`flute-synth.voice`, 1,536.

## dacvoice

```
make
dacvoice defs/flute.def flute.voice
```

C99 and the math library; keep `-ffp-contract=off` (the Makefile sets
it), so every host makes the same words. Without an output file the
voice goes to standard output.

A definition, a setting a line, `#` a comment:

```
name flute
family winds                optional; written to the voice file as it is
n 10                        the sample period: 20 kHz at 10
harmonics 1 1.0  2 0.3  3 0.12  4 0.05  5 0.03
                            (number, amplitude) pairs; the line may repeat
bright 0.3                  how much faster the upper harmonics fall in a
                            softer timbre: harmonic h at gain^(1 + bright (h - 1))
timbres 0 10 20             each timbre's level, dB down, loud to soft (1 to 4)
start 30 dB                 where a note starts, dB down
attack 0.045 s              from the start to the peak
peak 0 dB
decay 7.5 dB/s              from the peak to the sustain level
sustain 6 dB                or: sustain none (the decay runs on to silence)
release 0.045 s             the time to fall 30 dB after the key is let go
zones 6                     optional: no zone 6 (the default is 7, with it)
```

The units may be left out. What it makes:
- one 256-word period for each timbre, divided by that timbre's gain, so
  at its own attenuation it plays exactly that level; the device plays
  the timbre nearest the envelope's level, the thresholds halfway;
- zones of an octave from pitch 2 (C1), six in the map, each holding
  only the harmonics below 0.45 of the sample rate at its top pitch, so
  no note aliases; the map's top zone, from C6 (62), is cut for C#6
  (63), and zone 6, pitches 64-75, for C#7 (75), the highest a note an
  octave up sounds. With `zones 6` there is no zone 6, and the top zone
  is cut for B6 (73), where a note an octave up then sounds. Zones with
  the same harmonics share one block (the flute's and the marimba's zone
  6 is their zone 5);
- one scale for every zone, the loudest sample at 0.95 of full scale;
- the steps for every pitch, equal temperament from A4 = 440 Hz, and
  the envelope's latch values.

It refuses a definition whose softer timbres would clip, a step or a
level beyond the device's range, or a top zone with no harmonic left (a
smaller `n` raises the sample rate). It refuses, too, a rate that rounds
to 0 where its stage must move: the device holds a stage whose rate is 0,
and a rate is in units of 0.763 dB/s, so one under about 0.38 dB/s is 0.
That is an attack from a start under the peak, a decay of more than 0
dB/s to a sustain under the peak or to none, and any release longer than
about 79 s (30 dB over its time); the message names the stage. `decay 0
dB/s` with the sustain at the peak, as the Flentrop's stops have it, and
a start at the peak are taken. Sampled voices are not made here: the
four synthesized ones in `lib/` (`principal-synth.voice`,
`gedackt-synth`, `bourdon-synth` and `harpsichord-synth`) and the
recorded ones, the nine of `standard.lib`, the bagpipe's chanter and
the instruments of `../instruments/`, were made by tools that are not
part of the release.

## Stops

A definition with a `frame` is an organ stop. The player adds the stops
a converter sounds together when it loads them (the premix), so a
division's stops share its N, frame, envelope and chiff, written alike
in each of its definitions; `dacdrum` refuses a family whose stops
differ. In place of `bright` and `timbres`, which a stop may not have:

```
frame 8                     the division's frame in feet, 8 or 16; a stop in
                            a 16-foot frame sounds an octave under the pitch
                            the part names (the Pedaal at 8 FT sounds 16')
harmonics 1 1.0  2 0.55 ... the pipe's own harmonics
rank 6 1.0 break 4          where the pipe sounds: the frame harmonic of its
                            fundamental (1 the frame's pitch, 2 an octave up,
                            3 a twelfth ...), its level, and the zones (1-6;
                            6 only with zone 6) where it breaks back an
                            octave; may repeat, for a mixture. None: rank 1 1.0
rank 5 1.0 nyquist          a rank that cannot break back (an odd harmonic,
                            the Terts): its own pitch kept while under 0.5 of
                            the sample rate, its harmonics still cut at 0.45;
                            the Terts has `zones 6`, since at 75 its rank
                            would pass the Nyquist
level 2 dB                  under a quarter of full scale (optional, 0)
chiff 0.012 s 16 dB 2 16    the division's chiff: its decay to 1/e at each
                            zone's lowest pitch, its peak under a quarter of
                            full scale, its band in frame harmonics
```

What it makes:
- one 256-word table a zone: each rank's pipe on the frame's harmonics
  (harmonic k of a rank at frame harmonic H is the frame's k H), the
  zone keeping those below 0.45 of the sample rate at its top pitch, as
  a table voice's; zones with the same words share a block. A rank whose
  own pitch is not below a zone's limit (0.5 of the sample rate for a
  `nyquist` rank) is refused: the definition must break it back;
- one scale for every zone, the loudest sample at a quarter of 0.95 of
  full scale, lowered by the level, so four stops add to no more than
  1,944 of 2,047;
- each zone's speech, 1,024 words of chiff: 64 sinusoids at random
  frequencies in the band (cut at the zone's top harmonic), the same
  random numbers in every zone, under a short rise, the decay and a fade
  to 0. It is played at the note's step, so it lasts four of the note's
  periods at most. Its peak must leave four stops room: at most 103;
- the steps, an octave down in a 16-foot frame, and the envelope.

The premix, in the player and the renderer: for each of the six zones,
and on a drum of version 5 a seventh, a block of 1,280 words, word i
the sum of every stop's table word i mod 256 and, below 1,024, of the
first stop's speech word i, played as a sample voice whose loop is the
last 256 words. The seventh is zone 6 when every stop has one; if a stop
has none (the Terts), it sums the others' zone 6 tables and that stop's
zone 5, and pitches 62-63 play it in zone 5's place, so the premix has
no zone 6.

The Flentrop's are made with

```
for f in defs/flentrop/*.def; do ./dacvoice $f lib/flentrop/$(basename $f .def).voice; done
```

## The voicing

A table voice or a stop may carry the four lines above in its
definition, written to its voice file as they are (version 3, or 4 with
the céleste); a sample voice may not have the edge or the rise. The
player gives them with the note in `dzv`'s low nine bits (the device's
`IOTs/DAC/edge.h`; `../Implementation.md`, "The voicing"):
- `edge`, the edge tone: noise in a band about the converter's
  fundamental, quality 3, under the note's envelope, 4 dB more each
  octave under C4: the wind at a flue pipe's mouth;
- `rise 10`, the speech: the attack takes 10 of the note's periods unless
  the voice's own is slower, so a large pipe speaks more slowly, with
  breath through it (noise about twice the fundamental, 24 dB under the
  stop, dying away over the rise);
- `room busch`, Adolphus Busch Hall: with the emulator's `dacroom=voices`
  (and the renderer's `--room=voices`), the room at each go is Busch
  Hall if a running converter's note asks for it, the hall otherwise;
- `celeste 2`, the céleste: the converter reads its words a second time,
  at half the rate, through a second phase 2 cents sharp, the two summed
  at equal power: a rank a little off the unison, beating with the
  first. Each second read is one more cycle stolen. In `dzv`'s bits 9-12,
  the detune in half cents.

The Flentrop's 27 stops have `rise 10`, `room busch` and `celeste 2`;
the four flutes
that sound the converter's fundamental, Roerfluit 8, Holpijp 8, Zingend
Gedekt 8 and the Pedaal's Bourdon 16, have `edge -22` as well (set by
ear from -30, -26 and -22; the band
follows the converter's fundamental, so a 4 or 2 ft flute's edge would
sit under its pipe). A converter's stops combine as the player premixes
them: the loudest edge, the largest céleste, and the speech and the
room if any stop has them, and `dacdrum` does not hold the voicing to a family alike.

## Recorded instruments

A definition may give the converter's work for recorded instruments
(`../Implementation.md`, "Recorded instruments"), in its own units; any
of these lines makes the voice file version 5:

```
interpolate                 read between a sample and the next
vibrato 5.5 Hz 20 cents delay 0.3 s
                            the slow counter at 5.5 Hz, a pitch swing of
                            20 cents, starting 0.3 s into the note; or
                            vibrato 6.7 Hz 2 cents free, never reset
leslie 3 dB 6 dB            the tremolo's depth and the place's swing
                            between the channels; it needs a vibrato line,
                            whose rate it takes (0 cents for no pitch swing)
percussion 6 dB 4 decay 12 dB/s
                            a sine at 4 times the note's step, 6 dB under
                            it, decaying 12 dB a second
```

Each is written as its latch value: the rate F as F x 2^24 / 3,125, the
depth D as 16,384 (2^(D/1200) - 1), the delay in units of 5.12 ms, the
dB in eighths, and the decay in the envelope's rate unit. The
percussion's sine is written as a 256-word block after the others, at
the voice's own scale: 0.95 of full scale for a table voice, a quarter
of that for a stop, less its `level`. It refuses a vibrato rate under
0.0002 Hz or a decay under 0.38 dB/s (each rounds to 0), a multiple
outside 1 to 7, and a `leslie` without a `vibrato`. A family's stops
must be alike in all four lines, as in their envelope.
`../instruments/hammond-slow.def` and `hammond-fast.def` are stops of
this kind.

## dacdrum

```
make
./dacdrum lib/standard.lib ../drums/pdp23drum-mine
./dacdrum --version=5 lib/flentrop.lib ../drums/pdp23drum-mine
```

writes the whole drum: the directory in field 0, the voices in fields
1-31, zeros everywhere else. Written straight onto the emulator's
`/opt/pidp1-mods/pdp23drum`, it replaces everything on the drum, a
`zloader` story or your own programs included, so write it beside, as
above, and copy it in as `../UsingDAC.md` says, your own drum saved
first. It writes beside its output file and renames onto it, so a
running emulator takes a new drum whole at its next transfer: copied in
while the menu is up, the next PLAY finds the voices, with no restart.
`make` in `..` builds it and writes the six drums into `../drums/`.

It refuses, writing nothing: a library that does not fit the 31
fields, more than 64 voices (the player reads 56 at most, and refuses a
larger directory), a name that is not 1 to 17 letters,
digits, spaces and `/`, a family that is not 1 to 11, more than 16
families, a family of more than 22 voices (the rows the menu's sublist
shows), a voice the player could not load (more than 16 blocks, a
header longer than the player's slot of 190 words, a block longer than
a field's 4,096 words but on a version 5 drum, more than the 45,056 words of the player's banks
5-15, for a stop its premix's six blocks, seven on a version 5 drum; a stop whose speech is not
its speech block's length, whose tables are not 256 words, or whose
speech is shorter than the 446 words of another stop's header and table,
which the player reads there while it premixes), a family whose stops
are not alike in N, envelope, sample, thresholds, steps, speech and
version 5's lines (a family whose stops do not all have zone 6, alike in
zones 0-4's speech only, since their zone 5 speech is cut for C#6 or
B6), and any voice file the renderer would refuse.

A block whose words an earlier block has already put on the drum is not
written again: its header names the earlier block's place. So a
division's speech, the same in each of its stops, is on the drum once.

The drum file is 32 fields of 4,096 words, each word a 32-bit int in
the host's order with the 18-bit value in its low bits, as the drum's
plugin holds it. **The directory**, field 0 from word 0:

| word | holds |
|---|---|
| 0 | the magic word, 0414243 |
| 1 | the format's version: 3, or 4 when a voice has a voicing, or 5 when a voice file is version 5 or `--version=5` asks |
| 2 | the library's serial: the sum, as the PDP-1 adds, of fields 1-31 |
| 3 | V, the voices, 1 to 64 |
| 4 | the directory's sum: words 0-3 and 5 to the last voice's entry, added as the PDP-1 adds |
| 5 | F, the families, 1 to 16 |
| 6-69 | 16 families of 4 words, in the order the library first names them: each a name as 340 text (three 6-bit codes a word, as am1's `type340` packs upper case, the end code 037, then zeros); the unused ones 0. A voice with no `family` line is in OTHER; case does not count |
| 70 on | for each voice, in the library's order, 8 words: its name as 340 text in 6 (`/` is 057), where its header is (field << 12 \| address), and its family's number, 0 to F-1, with the sign bit, 0400000, set for a stop |

The player takes versions 3, 4 and 5, and refuses a drum written before
version 3 with NO VOICES ON THE DRUM; write it again with this
`dacdrum`. The player places a voice's blocks one after another from
050000 in core, each block of a voice with its own ends after three
words, its end, loop and stride; the renderer places them the same.

**A voice's header**, versions 3 and 4:

| words | holds |
|---|---|
| 13 | its record, the words the player latches from, but words 1 and 2 (the zone map's and the steps' addresses) are 0: the player fills them; word 12, threshold 3, holds the voicing in its low nine bits (`dzv`'s) |
| 1 | B, its blocks |
| 3 B | for each block: where it is (field << 12 \| address), its length, its sum as the PDP-1 adds |
| 64 | its zone map, as block numbers |
| 64 | its steps |
| 6 | a stop's alone: its speech map, the speech block of each zone |

A stop's record is the premixed voice's: sample mode, the end 1,280 and
the loop 256. The header and each block go whole into one field,
first-fit in fields 1-31, in the library's order, so each is one drum
transfer. The player reads every header as its whole slot of 190 words,
so a header is placed only where those 190 stay in the field; it takes
only its own words, and a block may follow it at once. The standard
library takes fields 1-18, the Flentrop library fields 1-29, 114,395
words.

**Version 5** is written when a voice file is version 5, or when
`--version=5` asks; a library without one is written in the lowest
version that holds it (4 if a voice has a voicing, 3 otherwise). Its directory is version 4's. A voice's header:

| words | holds |
|---|---|
| 13 | its record, as above; word 7's held actions with 040 for the interpolation and 0100 for the packing |
| 6 | `dyv`, `dyw`, `dym`, `dyk`, `dyj` (0 for a voice without them), and the percussion's block number |
| 1 | B, its blocks, with the sign bit set when each note latches its block's end, loop and stride (some block a zone names has its own) |
| 2 | where its steps are, and their sum |
| 6 B | for each block: where it is, its length, its sum, its end, loop and stride |
| 64 | its zone map, as block numbers |
| 1 | zone 6's block plus 1; 0 for a voice without zone 6 |
| 7 | a stop's alone: its speech map, zone 6's entry last (zone 5's again for a stop without zone 6) |

The steps are a block of 64 words of their own, written once like any
block, so a library's voices that share their steps share one set. A
header is 94 + 6B words for a stop, 87 + 6B for any other voice: at
most 190, the slot exactly, reached only by a stop of 16 blocks; the
largest on today's drums is 172. On a
drum of version 3 or 4 the word after the zone map is pitch 0's step,
which dacvoice writes as 0 in every voice (pitches 0 and 1 are rests),
so the player reads it as "no zone 6" without testing the version: a
voice file whose step 0 is not 0 would break that. A block of more
than 4,096 words runs from its place to its field's end and on from
word 0 of each field after it, in fields of its own but the last, whose
rest the next places may take; it goes in the first field whose free
end, with the empty fields after it, holds it. A block of a field or
less is still whole in one field.

The player checks the directory (magic, version, V, F, sum) at the menu's
start, at each hit on an instrument, at READ TAPE and the console's
`load`, and at PLAY; at PLAY it reads each voice's header and blocks
and checks every block's sum.
