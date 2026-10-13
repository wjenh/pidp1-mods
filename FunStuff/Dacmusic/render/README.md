# dacrender: a punched tape to a wav file

`dacrender` plays a music tape for the PDP-1's D/A converters into a
wav file, as the PDP-1 would play it, with no PDP-1, no emulator and no
Python. The tape is one the music program punched (PUNCH on its menu):
Peter Samson's Harmony Compiler parts, then the orchestra the menu set,
each converter's part, instrument, octave, volume and place. A tape with no
orchestra (a Harmony Compiler tape as it comes) plays the program's
default: an organ, principal, gedackt and bourdon, the bass doubled at
16 feet.

It is a straight rendering: 16-bit stereo at 50 kHz, the converters' own
rate, each converter at its place in the stereo field as the orchestra
sets it (one of nine, LEFT, L3, L2, L1, CENTER, R1, R2, R3, RIGHT, on a
constant-power law: CENTER is 3 dB down in each channel), with no mixing,
gain or trimming of its own. A tape with no places (no orchestra, or a
block punched before them) has converters 1 and 2 on the left and 3 and
4 on the right. Each channel is the converters' sum over 2, as the
device's: with three or four converters on one side, or all four
centered, a loud passage can pass full scale and is clipped, and the
count of clipped samples is printed. The file runs to the end of the
two-second pause after the last movement.

With `--room=`, the device's room is put after the mix, as the
emulator's `dacroom` setting puts it: `busch`, Adolphus Busch Hall at
Harvard, where the Flentrop stands (falling 60 dB in 4.1 s at 100 Hz,
3.4 s at 1 kHz and 1.9 s at 10 kHz, the first of the room 49 ms after
the note, a plate beside the hall); or `hall`, a generic hall of 3 s,
for every other voice. It is the device's own code
(`IOTs/DAC/room.h`), so the file is what the device plays. A room adds
to the peaks, so with one the
whole sound, dry and room together, is cut: 3 dB in Busch Hall, 6 dB in
the hall, enough for the loudest tapes of the voices each room is for
(the Flentrop's full plenum, the nine's Sonata I). The file then runs on
past the pause while the room dies away: 2 s more for the hall, 4 s for
Busch Hall. `--room=voices` lets the voices choose, as the emulator's
`dacroom=voices` does: Busch Hall if a voice the orchestra plays asks
for it (`room busch`, as the Flentrop's stops do), the hall otherwise;
it prints which.

A voice file may give the voice its voicing (below, "Voice files"): the
edge tone, the wind at a flue pipe's mouth, and the speech, a large pipe
speaking more slowly than a small one, with a breath of noise as it
does, and the céleste, a second read of the converter's words at half
the rate, a few cents sharp, so the note beats as a rank a little off
the unison does. They are the device's own code (`IOTs/DAC/edge.h`), so
they too are what the device plays.

A voice file of version 5 (below) gives the converter's work for
recorded instruments (`../Implementation.md`, "Recorded instruments"):
the interpolation between a sample and the next, a vibrato from the
slow counter, a Leslie's tremolo and its swing of the converter's place
between the channels, a percussion sine at a multiple of the note's
step, the packing of three samples in two words, and a block's own end,
loop and stride, with which a slur to another zone strikes the note
again. The renderer plays them as the player and the device do. Each is off
unless the voice asks for it.

With `--onset=MS`, each note in wavetable mode may start up to MS
milliseconds late (a note in sample mode starts on time), as
the emulator's `daconset` setting starts it: a tracker organ's pallets
do not open the instant the key goes down, and the pipes of a chord do
not speak together. The delay is drawn for each converter from its own
fixed sequence, so a tape renders the same every time, and it is never
more than half the next note's length. The timing of the piece does
not move: only where each note begins sounding within it. Without the
switch every note starts on time.

## Build

Any C99 compiler and its math library; nothing else. `dacrender.c`,
`dacrender_lib.h` (the nine voices, compiled in), `Makefile`, and the
device's `room.h` and `edge.h` in `IOTs/DAC`, the room and the voicing.
`make` in `..` builds it; here, on its own:

```
make
```

or by hand, from this directory:

```
cc -O2 -std=c99 -ffp-contract=off -I../../../IOTs/DAC -o dacrender dacrender.c -lm
```

The Makefile's `ROOMDIR` names the directory of `room.h` and `edge.h`
(`make ROOMDIR=path` if they are elsewhere). This builds on Linux (a
Raspberry Pi included), macOS, and Windows under WSL or MinGW. Keep
`-ffp-contract=off`: it makes every host compute the same samples.

## Run

```
dacrender tape.bin [out.wav] [--secs=S] [--voices=file.lib] [--room=hall|busch|voices] [--onset=MS]
```

The file is the tape's name with `.wav` unless one is given. `--secs=S`
renders only the first S seconds (and no room's tail after them). A whole trio sonata, about 16 minutes
of music, took 3.4 seconds on an x64 PC. `--voices=file.lib` plays the
voices that library lists in place of the nine compiled in (below,
"Voice files").

It prints the tape's parts and movements, the orchestra, volumes and
places it plays, the piece's length, the peak, the samples clipped and
the time it took. A tape the
program would question is played as the program plays it, with a
warning in place of the menu's message:
- a checksum that fails: the block is taken as read;
- an orchestra the program would refuse (an instrument it does not have,
  a volume that is not one of the slider's levels; in a version 2 block,
  also a stop the library lacks, a gap in the stops, a detune, or a
  place that is not one of the nine): the default plays;
- a tape error (a bad count, more than 30 parts, a tape larger than the
  program's two banks): the parts read whole before it play.

The orchestra block is read in each of the three forms a player's
PUNCH has written: version 2 (38 words, what this player punches), the
17-word block with volumes, and the 13-word block without them.

## MP3

50 kHz is not an MP3 rate; either encoder below resamples to 48 kHz on
its own.

```
lame -V 2 piece.wav piece.mp3
```

```
ffmpeg -i piece.wav -ar 48000 -b:a 256k piece.mp3
```

## Voice files

The instruments are voices: the nine the music program has are compiled
in, numbered 0-8 as the program numbers them, and an orchestra block
names them by those numbers. `--voices=file.lib` plays another library:
a list of voice files, one a line, relative to the list's own
directory, each voice numbered by its place in the list from 0 (blank
lines and `#` comments are not counted). The renderer places the voices
an orchestra plays in the converters' memory itself, so a voice file
holds no address. It does not read the drum file the program loads its
voices from (`../voices/README.md`, "dacdrum"); give it the same
library with `--voices`. The libraries here:

| library | the drum made from it |
|---|---|
| `../voices/lib/standard.lib` (compiled in) | `pdp23drum-standard` |
| `../voices/lib/flentrop.lib` | `pdp23drum-flentrop` |
| `../instruments/hammond.lib` | `pdp23drum-hammond` |
| `../instruments/recorded-1.lib` | `pdp23drum-recorded-1` |
| `../instruments/recorded-2.lib` | `pdp23drum-recorded-2` |
| `../voices/lib/bagpipe.lib` | `pdp23drum-bagpipe` |

A voice file is plain text, a field a line; a line that starts with a
number continues the field above it, and `#` starts a comment. Numbers
are decimal but a block's words, which are octal:

```
dacvoice 1
name flute
n 10                        the sample period, in machine cycles
mode table                  table or sample
envelope S P A D U R        the device's latch values: start, peak,
                            attack, decay, sustain (511: none), release
stride 256                  from one timbre's table to the next; 0 for one
thresholds 40 120 511       the followed timbre's levels; 511: none
sample E L                  a sample's end and loop (a sample voice only)
steps                       the step of every pitch, 64 numbers
zones                       the block every pitch plays, 64 numbers
block 0 768                 a block's number, from 0, and its length,
    056500 063400 ...       then its words
```

Version 2 adds the organ stop, version 3 the voicing and version 4 the
céleste:

```
dacvoice 4
edge -30                    the edge tone, dB under the stop at C4: even,
                            -34 to -22
rise 10                     the speech over 10 of the note's periods
room busch                  the room the voice asks for: busch or hall
celeste 2                   the céleste's second read, cents sharp: 0.5
                            to 7.5 in half cents
```

Version 5 adds the recorded instruments' converter:

```
dacvoice 5
interpolate                 read between a sample and the next
vibrato 29528 190 59        the slow counter's rate, the vibrato's depth
                            and delay (or free)
leslie 24 48                the tremolo's depth and the place's
percussion 24 2 26 3        its level, its multiple, its decay and its
                            sine's block
block 0 7200 2400 800 2400  a block's own end, loop and stride
pack                        three samples in two words, the ends in
                            groups
zone6 1                     zone 6's block, for the notes an octave up
```

(`../voices/README.md` has them all.)

A file the renderer cannot read, or whose blocks are shorter than the
voice plays, ends the run with the file and line.
