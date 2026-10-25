# Using Harmony, the Harmony Compiler

This document describes Harmony, Peter Samson's Harmony Compiler for the PDP-1 music tapes, rebuilt in am1.

This is version 1.0\
Edit date 24-Oct-2026
Make punch behavior clear.

## What is it?

In the early 1960s Peter Samson wrote a music system for the PDP-1.
A piece is typed as a source tape, a few bars to a line, one part at a time, in a small language of notes,
rests and bar lines.
The Harmony Compiler, *hc1d*, turns the source into an intermediate tape of numbers, and Samson's player,
*pdp1m13*, plays that tape on the four program flags.
Both programs are in */opt/pidp1-mods/tapes/music*, *hc1d.mac* and *hc1d.rim*, *pdp1m13.mac* and *pdp1m13.rim*.

Harmony is *hc1d* again, written in am1.
For every source it punches the same words *hc1d* punches and types the same text on the typewriter.
Where *hc1d.mac* and *hc1d.rim* differ, Harmony does what *hc1d.rim* does, since that is the program that ran.

What it adds:
- **No halts.** *hc1d* compiles one part, punches it and halts, and Continue compiles the next.
  Harmony reads the whole tape in one run, keeps every part in core and punches them all at the end.
- **More room.** A part may be larger than *hc1d* allows.
- **Three inputs.** A FIO-DEC source tape as *hc1d* reads, a plain ASCII text file, or a compiled tape.
- **Three languages.** *hc1d*'s, the 1968 language of the Haffner Symphony's source tape, and 1968 DAC, which
  also punches the score's dynamics for the DAC music player.
- **A menu** on the Type 340 display, operatd with the lightpen, that lists the parts, shows what was typed for
  each, changes the tempo, and punches.
- **SOURCE**, which punches the parts in core back out as a source tape that compiles to the same words.
  It works on a compiled tape too, so any intermediate tape can be turned back into source and edited.

The tapes Harmony punches play on *pdp1m13* and on the DAC music player, *dacmusic.rim*, in
*/opt/pidp1-mods/FunStuff/Dacmusic*, see its *UsingDAC.md*.

## What's here?

| Where | What |
|---|---|
| *FunStuff/Dacmusic/harmony* | Harmony, *harmony.am1* and the files it includes, and its *Makefile* |
| *FunStuff/Dacmusic/midi/midi2hc* | A MIDI file to source text for Harmony, see its *README.md* |
| *tapes/music/hc1d.mac*, *hc1d.rim* | Samson's compiler, the reference for its language |
| *tapes/old/mozart_sym35_1feb68.bin* | The Haffner Symphony's source tape of 1968, in the 1968 language |
| *FunStuff/Dacmusic/tapes/haffner-1968.bin* | That source compiled in 1968 DAC, with an orchestra for the DAC player |

## Building it

*make* in */opt/pidp1-mods/FunStuff/Dacmusic* builds Harmony with the rest of the DAC music programs.
To build Harmony alone:
```
cd /opt/pidp1-mods/FunStuff/Dacmusic/harmony
make
```
That makes *harmony.rim*.

## Running it

Harmony uses all sixteen banks of core.
Its typed output goes to the typewriter, as *hc1d*'s does.
The menu mode needs the Type 340 and the lightpen, so set *lightpen=on* in *pidp1.config*.

Load *harmony.rim* with **fastload** or **pdp1central**'s Load..., or read it in from the paper tape reader.
It has two starts,
**fastload** and **pdp1central** start the program at 100 after loading it.

### The console start, 101

The console start does what *hc1d* does, without the halts.
Mount the source tape on the reader, set the test word, and start at 101.
Harmony reads the whole tape, compiling each part and typing what *hc1d* types, punches every part it, and
halts.
To compile another tape, mount it and press Continue.

The test word, read at each start:

| Test word | What it does |
|---|---|
| 0 | *hc1d*'s language |
| 1 | The 1968 language |
| 3 | 1968 DAC |
| 0700 | With any of those, punch nothing, as *hc1d* does |

Bit 16 alone, 2, is also *hc1d*'s language.

### The menu's start, 100

Start at 100 for the menu on the Type 340.
The test word chooses the initial language, LANGUAGE on the menu changes it.
No tape is read until you select READ TAPE, and the punch is only used if PUNCH or SOURCE is selected.

## The menu

The top of the screen holds HARMONY COMPILER and the tempo control, then the commands, READ TAPE, PUNCH,
LANGUAGE and LOG.
Under them is the status line, which indicateswhat just happened, then a line of counts, then the parts, 16 rows
a page.
The bottom line has MORE, PAGE p OF q, SOURCE and FORM.

Pen-down on a command to select it.
A selection acts when the pen leaves the selection, which means mouse-button-up, and only once.

### The commands

- **READ TAPE** reads the tape on the reader and compiles it, replacing any parts read before.
  The rows fill in as the parts come in and the typewriter types what *hc1d* would.
  The status line says READING TAPE, then TAPE READ.
- **PUNCH** punches every part kept that has words, see *What it punches* below.
  The status line says PUNCHING TAPE, then TAPE PUNCHED, or TAPE PUNCHED, FOR THE DAC PLAYER when the tape was
  read in 1968 DAC.
- **LANGUAGE** steps through HC1D, 1968 and 1968 DAC.
  It takes effect at the next READ TAPE and the next SOURCE, not on the parts already read.
- **LOG** opens the log, everything typed since the last READ TAPE.
- **SOURCE** punches the parts as a source tape, see *SOURCE* below.
- **FORM** chooses SOURCE's form, FIO-DEC or ASCII.
  It is FIO-DEC at each start.

While READ TAPE and PUNCH run, the screen stays up, but it flickers while a character types.

**IMPORTANT!** Wait for punching to finish here before saving the punch output to a file, as via *pdp1central*.
The emulator puts the punch output into a temporary file and until punching is complete, saving the output will
save only what has been currently written to the temporary file.

### The tempo

After READ TAPE the tempo readout shows TEMPO and the number of the first tempo word in the parts, or TEMPO NONE
when there is none.
A tempo word is a length, as *pdp1m13* reads it, so a larger number plays slower; 170 is *pdp1m13*'s
default.

-10, -1, +1 and +10 change the tempo number to punch, within 1 to 32767.
While it differs from the tape's the readout shows both, TEMPO 180 FROM 170.
At PUNCH every tempo word in every part is scaled by the same proportion, rounded, so the tempo changes within
the piece keep their relationships.
If any word would be outside 1 to 32767, nothing is punched and the status line says
NOT PUNCHED: A TEMPO WORD WOULD LEAVE 1-32767.
Once punched, the parts in core hold the new tempo, a second PUNCH gives the same tape.

The console start never changes a tempo.

### The parts and the log

The counts line shows PARTS and the number of parts kept, then, if there are any, GIVEN UP, CUT SHORT,
BAD PARITY and NO FIO-DEC CODE, see *Messages and limits* below.

Each row is a part, its number, the first 16 characters of its title, its BARS, NOTES, and ERRORS.
A compiled tape's parts have no title and no errors.
MORE shows the next page of parts.

Pen-down on a row and the log opens at that part's typed text.
The log shows 40 lines a page, 66 characters to a line.
MORE pages on, then from the last page back to the first.
BACK returns to the parts.

## The three inputs

READ TAPE and the console start tell the three apart from the first frames after the leader.

**A FIO-DEC source tape** is read as *hc1d* reads it.
A frame with bad parity is skipped and counted, where *hc1d* halts on it.

**An ASCII text file** is a source too.
Write commands in lower case.
Each character becomes its FIO-DEC code with the case shifts put in for you, so an upper case run is a comment
as on a FIO-DEC tape, and *VIOLIN I* is two comments with a space between them.
- The digits and / , . ( ) - are lower case; " ' ~ < > ? = + [ ] | _ are upper case.
- A space, a tab, and a line end are each a space to the compiler.
  A carriage return, a line feed, or the two together end a line.
- NUL characters are leader.
- A character with no FIO-DEC code is typed in red as *nfd* and its octal value, counted, and skipped.

*hc1d* loses the first character after a part's *end* line when Continue starts the next part and Harmony loses
it also, so the text is compiled as *hc1d* compiled it.
A FIO-DEC tape has a stop code written.
In a text file, leave an empty line after each *end* line.

**A compiled tape**, one *hc1d* or Harmony punched, is checked part by part as *pdp1m13* reads it, the counts and
the checksums, and each part is listed on the typewriter, *part 1, notes 706, bars 63*.
The parts are kept for SOURCE, but PUNCH punches nothing from a compiled tape.
An orchestra block from the DAC music player is skipped over and the typewriter says so.
The status line says COMPILED TAPE, CHECKED, or COMPILED TAPE REFUSED, SEE THE LOG.

A tape that is none of these is refused, NOT A TAPE HARMONY READS.

## A MIDI file as source

*midi2hc*, in */opt/pidp1-mods/FunStuff/Dacmusic/midi* and built by the same *make* turns a Standard MIDI File
into an ASCII text file in *hc1d*'s language, one part to a track, ready to mount on the reader:
```
cd /opt/pidp1-mods/FunStuff/Dacmusic
midi/midi2hc ~/minuet.mid ~/minuet.txt
```
Every bar is on its own line and numbered, every accidental is written on its note, and the file's tempo changes
are tempo words in part 1, so the text is easy to read and change before it is compiled.
Each part has one note at a time; a file that plays chords in a part is refused, with the part and bar.
*--played* takes a file played in by hand, snapping it to 64ths, and *--tempo N* sets the first tempo word.
Its *README.md* gives the rules, the summary it prints and its refusals.

## The languages

### hc1d's language

Samson's language is described in his specifications for the coding, attached to Donald Sordillo,
*Music Playing on the PDP-6*, MIT Artificial Intelligence Project Memo 107 (Project MAC MAC-M-326),
22 August 1966.
The memo describes a version a little different from *hc1d*, so where they disagree *hc1d.mac* is the reference.
Harmony takes the language exactly as *hc1d.rim* does, oddities included.
For example, the last articulation letter in a note wins, and a part's last bar with no slash before *end*
types its errors past *end*.

### The 1968 language

The language of the Haffner Symphony's source tape, *tapes/old/mozart_sym35_1feb68.bin*, dated February 1968,
a later form of Samson's language known only from that tape.
It is *hc1d*'s language with these differences:
- The title is the part's first line, no slash is needed after it.
- *units* is 32 at the start of each part.
- *append* and *stop* end a part as *end* does, and *stop* also ends the tape.
- *key* followed by *(* or *-* repeated k times is k sharps or flats, *key ((* for two sharps.
- An upper case vertical bar, *|*, starts a comment that runs to the end of its line.
- A slash after *rest N* or *copy M N* adds no bar.
- A bar number k bars ahead of the count repeats the k bars before it, as *copy* would.
  The 1968 tape writes a minuet's repeats this way.
- *tempo N* is a rate, N quarter notes a minute, or N half notes a minute when N is below 60.
  Harmony punches the tempo word *pdp1m13* plays at that rate.
- *trill N*, N a power of two up to 32, sets the length of the notes that the trill embellishments *m*, *n* and
  *w* make, a 32nd in *hc1d*.
  A *trill* shorter than a 32nd, or not a plain length, is skipped, so no trill plays faster than *hc1d*'s.
- The dynamics *pp p mp mf f ff*, and *open* and *close*, which mark the repeats, are skipped.
- Every note is compiled an octave lower than *hc1d*'s language would place it.
  The tape's music reaches above the top of *hc1d*'s range, so a 1968 tape is played an octave up, at 4 FT on the
  DAC music player.

The words skipped are counted, not typed as errors.

### 1968 DAC

The 1968 language with the dynamics punched.
Each of *pp p mp mf f ff* becomes a word in the part before the next note the part plays and the DAC music
player plays the part louder or softer from there, see *Loudness* in *UsingDAC.md*.
A dynamic before a *copy* puts the copy's first bar into the tape again with the dynamic at its start.

A tape with dynamics is for the DAC music player only.
*pdp1m13* halts at the first dynamic.

## What it punches

PUNCH, and the console start at its end, punch each part as *hc1d* does, the notes with their count and checksum,
then the bars with theirs, with blank tape between the parts and at each end.
The words are the same as *hc1d*'s so the tape plays on both *pdp1m13* and on the DAC music playere
unless it was read in 1968 DAC.

The tape has no orchestra.
On the DAC music player you set the orchestra on its menu and its PUNCH adds it to the tape.

To keep a punched tape, save the punch to a file with **pdp1central**'s Save punch to... or with
*/opt/pidp1-mods/bin/save_ptp.sh* after the punching is complete.
The punch runs at the *punchspeed* set in *pidp1.config*, the default being the original, accurate, speed.
However, for a long tape, be prepared to wait a long time.

## SOURCE

SOURCE punches the parts in core as a source tape in the language LANGUAGE shows and the form FORM shows.
Harmony compiles that tape back to the same words.
SOURCE can turn a compiled tape into source, or rewrite a source in one of the other languages.

It is not the original text.
A compiled tape keeps no titles, comments, or *key*, *up* and *down*:
- Each part's title is *part 1*, *part 2* and so on.
- The clef is the one that suits the part's range.
- The notes are spelled from the bar's accidentals.
- A bar that repeats earlier bars becomes *copy*, and a run of rest bars *rest*.

As it punches, the typewriter types a line for each part, *part 3, bars 120, copied 8*, with *changed* and a
count when some words had to be written differently, such as a tempo in the 1968 language that has no exact rate
or a dynamic in *hc1d*'s language, which has none.
A part SOURCE cannot write is refused with the reason, *part 5, refused at bar 40, a word no source makes*, and the
others are punched.
The status line says PUNCHING SOURCE, then SOURCE PUNCHED, or SOURCE PUNCHED, SEE THE LOG when anything was
changed, refused, or left out.

Each character is a frame of tape, so SOURCE takes a while, (quite) a few minutes for a symphony.
The console start has no SOURCE.

## An example, the Haffner Symphony

The repository has the source tape of Mozart's Haffner Symphony, K. 385, punched in February 1968 in the 1968
language, sixteen parts, four for each movement.
Here it is compiled for the DAC music player and played.

1. Load *FunStuff/Dacmusic/harmony/harmony.rim*, set the test word to 3, and start at 100.
   LANGUAGE shows 1968 DAC.
2. Mount */opt/pidp1-mods/tapes/old/mozart_sym35_1feb68.bin* on the reader and select READ TAPE.
   It takes about four minutes; the typewriter types each part's title, and there are no errors.
   The status line ends at TAPE READ with PARTS 16, and the tempo readout shows TEMPO 81, the first movement's.
3. In **pdp1central**, Save punch to... a file, *haffner.bin* say, then select PUNCH.
   The status line says TAPE PUNCHED, FOR THE DAC PLAYER.
4. Load *FunStuff/Dacmusic/music/dacmusic.rim* with a drum that has the nine basic voices, mount *haffner.bin*
   and select READ TAPE.
5. Set PARTS A MOVEMENT to 4, and the converters to parts 1 to 4 on PRINCIPAL, GEDACKT, FLUTE and BOURDON, each at
   4 FT, then PLAY.

*FunStuff/Dacmusic/tapes/haffner-1968.bin* is this same punch with that orchestra already on it, the principal
and gedackt on the left, the flute and bourdon on the right.
The renderer plays it to a wav file with no PDP-1:
```
cd /opt/pidp1-mods/FunStuff/Dacmusic
render/dacrender tapes/haffner-1968.bin haffner.wav
```

To see SOURCE at work, set FORM to ASCII and select SOURCE with the Haffner still in core.
The text it punches is the whole symphony in the 1968 language, and READ TAPE on it gives the same parts.

## Messages and limits

### What is typed

Everything *hc1d* types, Harmony types the same way.
- Each part's title, as it begins.
- At a part's first error, in red, *To err is human---to forgive, divine.*
- For each error, the measure it is in, then a tab and three letters.
  The letters are red for an error, where the word is dropped or skipped, and black for a complaint, where the
  note is kept or changed.

| Letters | Meaning | What happens |
|---|---|---|
| air | An accidental or embellishment on a rest | The rest as written |
| aor | An accidental takes the pitch out of range | The note becomes a rest |
| bbl | A bar number that is not the bar's count | The number is ignored |
| blc | *copy*'s first number is 0, or not below the bar | Nothing copied |
| brc | *copy*'s second number is below its first | Nothing copied |
| dtu | A dot on a 64th | The dot adds nothing |
| eit | An embellishment on too short a note | The note plain |
| eor | An embellishment's neighbor out of range | The note plain |
| ert | A duration not 1, 2, 4, 8, 16, 32 or 64 | The word dropped |
| etr | An embellishment on a triplet | The note plain |
| ilc | *copy* in a bar that has notes | Nothing copied |
| ilr | *rest* in a bar that has notes | No rest bars |
| itg | Grace notes with too little time after them | The grace notes dropped |
| mtl | A bar too long | As written |
| mts | A bar too short | As written |
| nor | A natural with a sharp or flat | The natural |
| nps | No such pseudo-op | The word skipped |
| tff | Too few fields | The word dropped |
| tic | A dot with a comma | The copied time |
| tmc | More than one comma | As one |
| tme | More than one embellishment | The last |
| tmf | Too many fields | The word dropped |
| tmg | More than one *g* | As one |
| tmr | More than one *r* | As one |
| uat | A pitch out of range | The note becomes a rest |
| unc | A comma with no note before it in the part | The word dropped |

Two messages give a part up, where *hc1d* halts:
- *Table overflow.  Subdivide source program.* when a part has more than 2,048 bars, or the parts no longer fit
  in core.
- *Measure has too many characters.  Rearrange tape.* when a title reaches 384 characters, or the source runs
  384 characters with no slash after the 256th.
  *hc1d* reads the source a stretch at a time, and from the 256th character on the first slash ends a stretch.

The part is dropped, GIVEN UP is counted, and Harmony compiles a new part from where the reader stands.
A tape that ends in the middle of a part drops that part too, counted as CUT SHORT.

The lines Harmony adds for a compiled tape or a tape it refuses:

| Typed | Meaning |
|---|---|
| compiled tape | The tape is a compiled one, listed part by part |
| orchestra block | The DAC music player's orchestra, passed over |
| a count that cannot be, refused | A block's count is 0 or past 32767 |
| a checksum fails, refused | A block's checksum is wrong |
| the tape ends inside a part, refused | The tape stops in the middle of a block |
| too large for the store, refused | The parts don't fit in core |
| not a tape harmony reads, refused | Not FIO-DEC, ASCII or a compiled tape |
| no tape | Nothing came from the reader |
| more than 128 parts, the rest not read | The tape has more parts than Harmony keeps |

A compiled tape refused keeps the parts read before the refusal.

### The status line

| Status | Meaning |
|---|---|
| READING TAPE, TAPE READ | A READ TAPE under way, and done |
| NO TAPE | Nothing came from the reader in about 10 seconds |
| NOT A TAPE HARMONY READS | Not FIO-DEC, ASCII or a compiled tape; nothing is kept |
| COMPILED TAPE, CHECKED | A compiled tape, every part's counts and checksums good |
| COMPILED TAPE REFUSED, SEE THE LOG | A compiled tape refused; the log says why |
| PUNCHING TAPE, TAPE PUNCHED | A PUNCH under way, and done |
| TAPE PUNCHED, FOR THE DAC PLAYER | Done, and the tape has dynamics |
| NOTHING TO PUNCH | No part has words, or the tape was a compiled one |
| NOT PUNCHED: A TEMPO WORD WOULD LEAVE 1-32767 | The tempo chosen is too far from the tape's |
| PUNCHING SOURCE, SOURCE PUNCHED | A SOURCE under way, and done |
| SOURCE PUNCHED, SEE THE LOG | Done, with a part refused, words changed or an orchestra left out |

### Halts

The console start halts once, after PUNCH; Continue reads the next tape.
There is no other halt.

### Limits

- 128 parts on a tape.
- 2,048 bars in a part.
- 49,152 words for all the parts together, the notes and bars as punched.
- A measure of 384 characters, as *hc1d*.
- The pitches, durations and tempo words of *hc1d*'s tape format, up to C6, a 64th, and 1 to 32767.
- The log keeps the first 2,048 characters typed after a READ TAPE.
- The reader is done when no frame comes for about 2 seconds, or about 10 seconds at the start of a tape.
