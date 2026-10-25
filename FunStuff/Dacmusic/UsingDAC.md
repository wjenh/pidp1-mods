# Using the D/A converters and the DAC music player

This document describes the PDP-1 D/A converters, IOTs 71 and 72, and the music player and tools built on them.\
How it all works inside, the IOTs included, is *Implementation.md* in this directory.

This is version 1.3\
Edit date 24-Oct-2026
Make punch behavior clear.

## What is it?

It's four 12-bit digital to analog converters for the PDP-1, each playing its samples straight from core memory
through a high speed channel along with a music program that plays Peter Samson's Harmony Compiler tapes on them
with real instrument voices.

There never was exactly such a device on a PDP-1, but there could have been.
DEC sold D/A converters, a 5 usec conversion was period hardware, and a
PDP-1 was exactly the kind of machine someone would hang a set of them on, through a high speed channel,
just to see what it could do.
Everything the device does is something 1960s logic could do: counters, latches, adders,
multiplying converters and one shared multiplier.

In fact, part of this was actually implemented, the 4 channel DAC and possibly more, by John Chowning on
the Stanford PDP-1D #48 in the '60s and used for researching this very application.
Also, early music synthesizers that were being developed in the late '60s used some of the same techniques,
but mostly implemented in analog circuitry, digital was still too expensive.

This does mean that this experiment would have cost a fortune, almost certainly more than the PDP-1 itself.
But again, it *could* have been done, therefore it's legitimate.

Finally, the program isn't just spitting out a digitized song.
It is playing the music in real-time using stored wavetables and configuring the DACs on the fly.
Some of the instrument waveforms did come from digitized real sound, some were completely synthesized
here using tools included.
You can create your own instruments and use them.

## A plea

Please don't use the pathetic little speakers on the pidp-1 panel!
They reproduce sound about as well as a tin can and string. A dented can at that.

Plug some decent PC speakers into the USB dongle or do what the Computer History Museum does on the real PDP-1,
feed the dongle into a real amp and speakers.
And consider upgrading the dongle, it's not great either and better ones are cheap.

If you don't want to go through the hassle, you can find mp3 versions for some of them in:\
https://drive.google.com/drive/folders/1BtgZoSKmaCbC2deI5yjbnqnxJUJDnQRe?usp=drive_link

## What you get
- **The device**, IOT 71 and IOT 72, four converters on high speed channel 2, in stereo
- **The player**, *dacmusic.rim*, which reads a Harmony Compiler tape and plays it with a menu on the
  Type 340 display that you use to configure playback using the light pen
- **The voices**, instruments the player loads from the Type 23 drum: the basic nine, all 27 stops of the
  Flentrop organ in Harvard's Busch Hall, seven recorded instruments, 42 Hammond organ presets, and a Highland bagpipe
- **The renderer**, *dacrender*, which turns a tape into a wav file with no PDP-1 at all
- **Harmony**, *harmony.rim*, Samson's Harmony Compiler rebuilt, which compiles music source tapes for this player and
  Samson's, see */opt/pidp1-mods/Docs/UsingHarmony.md*
- **Several tapes** are ready to play, including Bach's Little Fugue, *Wachet auf* and Toccata after E. Power Biggs's
  recordings

## What's here?

| Where | What |
|---|---|
| *IOTs/DAC* | The device, *IOT_71.c* and *IOT_72.c*, and *room.h* and *edge.h* |
| *Am1Includes/DAC/dacdefs.ah* | The device's mnemonics for am1 |
| *FunStuff/Dacmusic/music* | The player, *dacmusic.am1* and its menu |
| *FunStuff/Dacmusic/voices* | Nine voices and the Flentrop's stops, *dacvoice* to make a voice, *dacdrum* to put voices on the drum |
| *FunStuff/Dacmusic/instruments* | The recorded instruments and the Hammond presets |
| *FunStuff/Dacmusic/drums* | The voice drum images, made by *make* |
| *FunStuff/Dacmusic/render* | The renderer, *dacrender* |
| *FunStuff/Dacmusic/midi* | *midi2dac*, a MIDI file to a tape, and *midi2hc*, a MIDI file to Harmony source |
| *FunStuff/Dacmusic/harmony* | Harmony, Samson's Harmony Compiler rebuilt, see *Docs/UsingHarmony.md* |
| *FunStuff/Dacmusic/tapes* | Seven tapes to play |
| *FunStuff/Dacmusic/dac.config* | The settings to add to *pidp1.config* |
| *FunStuff/Dacmusic/CC-BY-SA-2.5-SE.txt* | The license of four of the nine voices, see *Credits* |

## Getting ready

### Build it

Follow *INSTALLATION.md*, *make install* in */opt/pidp1-mods/IOTs/DAC*, then *make* in
*/opt/pidp1-mods/FunStuff/Dacmusic*.

That builds the player, *music/dacmusic.rim*, Harmony, *harmony/harmony.rim*, the tools, the renderer, *midi2dac*,
*midi2hc* and six voice drum images in *drums/*.

### The settings

Add the lines of *dac.config* to */opt/pidp1-mods/pidp1.config*.
The plugin reads its settings once, the first time a program uses it, so if you change settings,
restart the emulator.

| Setting | What it does |
|---|---|
| lightpen=on | The player's menu needs the light pen |
| dacgain=1.0 | The converters' output level; 1.0 plays as the tapes were made |
| dacroom=voices | The room the music is heard in, see *The room* below |
| daconset=8 | An organ note starts up to 8 ms late, as on a real tracker organ, see below |

These two are where the converters start. A program can change either, and the player does when a tape or
its menu says to, see *The room and the onset* below.

The converters open their own audio device.
The emulator's *audio* setting and **pdp1audio** do not apply to them, so nothing else needs turning on.

### The drum

The player loads its voices from the Type 23 drum so the drum has to hold a voice library.

**WARNING!** - the drum is one file, */opt/pidp1-mods/pdp23drum*, and a voice drum replaces all of it.
Anything you keep on the drum, programs for **Rotator** or **programSelector** for example, is gone.
So save your own drum first:
```
cp /opt/pidp1-mods/pdp23drum ~/pdp23drum-mine
```
Then copy in the voice drum you want, the Flentrop drum is the best place to start:
```
cp /opt/pidp1-mods/FunStuff/Dacmusic/drums/pdp23drum-flentrop /opt/pidp1-mods/pdp23drum
```
And when you're done with the music, put yours back:
```
cp ~/pdp23drum-mine /opt/pidp1-mods/pdp23drum
```
The emulator can be running while you do this.
The drum reads the file again at its next transfer, so you can even change drums while the player's
menu is up, the next PLAY finds the new voices.
Just don't do it while a piece is loading or playing.

The six drum images:

| Drum | Voices | Plays |
|---|---|---|
| pdp23drum-flentrop | The base nine (0-8), then the Flentrop's 27 stops (9-35) | Every tape here |
| pdp23drum-standard | The base nine | *sonata1-stereo.bin* and Samson's tapes |
| pdp23drum-hammond | The base nine, then the 42 Hammond presets | *sonata1-stereo.bin* and Samson's tapes, and the presets from the menu |
| pdp23drum-recorded-1 | Flute, violin, string section, trumpet, horn | Instruments you pick from the menu |
| pdp23drum-recorded-2 | Flute, piano, guitar, drum kit, B3 slow, B3 fast | Instruments you pick from the menu |
| pdp23drum-bagpipe | The Highland bagpipe's chanter (0) and drones (1) | *scotland-brave.bin* |

The recorded instrument waveforms are too big for one drum, hence two.

A punched tape names its instruments by their numbers in the library.
A tape plays on any drum that has the same voices at the same numbers.
The first nine voices of the Flentrop and Hammond images are the standard nine, so those play anything the
standard drum does.
The Flentrop's nine are the synthesized ones, so a tape on the nine sounds different there, see *The nine basic
voices* below.

## Playing a tape

### Load the player

Load *music/dacmusic.rim* with **fastload** or **pdp1central**, or read it in from the paper tape reader and
start it at 100.

The menu appears on the Type 340:
```
  DAC MUSIC   9 PARTS   3 MOVEMENTS           PARTS A MOVEMENT 3

                                                                     '
  1 LEFT        PART 1    PRINCIPAL           8 FT    ................
                                                                     0
                                                                     '
  2 LEFT        PART 3    BOURDON             8 FT    ................
                                                                     0
                                                                     '
  3 RIGHT       PART 2    GEDACKT             8 FT    ................
                                                                     0
                                                                     '
  4 RIGHT       PART 3    BOURDON             16 FT   ................
                                                                     0
  READ TAPE     PLAY      PUNCH     ROOM CONFIG       ONSET CONFIG
  TAPE READ
```
Each row is one converter, its place in the stereo field, the part of the tape it plays, its instrument,
its octave and its volume.
The last line is the status line, it tells you what just happened.

The orchestra shown is the default, an organ with principal and gedackt on the upper parts, the bourdon on the bass,
and the bass doubled an octave down as a 16-foot pedal.

### Read a tape

Mount the tape on the paper tape reader as you would any tape, then select READ TAPE.

The header counts the parts as they come in.
When no more come for about 2 seconds the read is over and the status line says TAPE READ.
A tape this program punched also carries its added settings, the room and the onset among them.
These will not be seen by the original Samson player, so a tape created here will still play
on the original player.

READ TAPE doesn't halt, unlike the Samson player.
Any problem is reported on the status line, see *The messages* below.

A tape over 7,424 words, the whole Haffner Symphony for one, runs on into the banks the voices use, and the voices
load after it.
It plays as any other, but leaves less room for the orchestra, see VOICES DO NOT FIT WITH THIS TAPE below.

### Set the orchestra

Pen-down on a list opens it.\
In a list:
- pen-down at a row to select it, then at the OK box beside it to choose it
- DONE, at the top, or the list's header closes the list
- CANCEL closes it with nothing changed

The pen has to leave the screen or move off the list to select, which means mouse-button-up.

The fields:
- **The label**, *1 LEFT*, is the converter's place in the stereo field: LEFT, L3, L2, L1, CENTER, R1, R2, R3 or
  RIGHT. A part moved across the field keeps its loudness.
- **The part** is which part on the tape the converter plays, or NONE.
- **The instrument** list shows the drum's voices in families: ORGAN, WINDS, PLUCKED, etc., each family
  ending in *>*. Select a family to open its voices beside it.
  Pen-down on a selected voice again and the converter plays middle C on it for a second so you can hear it
  before you choose it.
- **The octave**: 8 FT plays the part as written, 16 FT and 32 FT one and two octaves down, 4 FT and 2 FT one and
  two octaves up.
- **PARTS A MOVEMENT**, in the header; how many of the tape's parts make up one movement.
  Samson's trio sonatas have three parts a movement, the movements play in order, two seconds apart.

### The room and the onset

ROOM and ONSET, on the line with READ TAPE, PLAY and PUNCH, are the room the music is heard in and how late
an organ note may start (see *The room* and *The onset* below).
Each opens its list as the other fields do.
- **ROOM** offers CONFIG, NONE, HALL, BUSCH HALL or VOICES.
- **ONSET** offers CONFIG, OFF, 2 MS, 4 MS, 6 MS, 8 MS, 12 MS or 16 MS.

CONFIG is whatever *dacroom* and *daconset* say in *pidp1.config*.
The converters take a choice at once, so an instrument you try in its list is heard in the room you chose.
Both are saved on the tape when you PUNCH, and READ TAPE sets them from a tape that has them.
A tape punched by the 1.0 player, or made elsewhere, sets both to CONFIG; a tape with no orchestra leaves them
as they were.

### Organ stops

The Flentrop's stops, and the Hammond presets, are organ stops.
Up to four of them from one family sound together on a converter, as the stops of an organ division do.
In a division's list, OK on a stop toggles it, and the converter's stops are marked *\**.
The field then shows the first stop and how many more, *PRESTANT 8+3* for four.
Use DONE to keep the stops you've set.

Stops of different divisions or a stop with a voice that is not a stop cannot share a converter.
PLAY says STOPS DO NOT MATCH.
To couple two divisions, give the same part to two converters.

### The volume sliders

To the right of each row is its volume slider, 16 points from 0 dB at the right to -45 dB at the left, 3 dB a
point, with the level in dB under it.
Pen-down at a point to set the level, or drag along the track.
The sliders work on the menu and while a piece plays and the level is saved on the tape when you PUNCH.
On a tape with dynamics the slider sets the level at f, and the tape's dynamics move it from there, see *Loudness*.

### Play it

Pen-down on PLAY.
The status line says LOADING VOICES while the voices come off the drum, a few seconds at most, then the piece
plays.
While it plays the screen shows the orchestra, the sliders and STOP; pen-down on STOP to end the piece
at once.
At the end the menu comes back.

Sense switch 1 on at the end plays the piece again.
A test word of 040 to 01377 scales every tempo as Samson's own player does: 0252, 170 decimal, plays as written,
a larger number slower.

Stopping the PDP-1 stops the converters too, and they don't resume on Continue.
Start again at 100, the menu comes back with the orchestra as it was.

### Punch it

**IMPORTANT!** Wait for punching to finish here before saving the punch output to a file, as via *pdp1central*.
The emulator puts the punch output into a temporary file and until punching is complete, saving the output will
save only what has been currently written to the temporary file.

PUNCH punches the tape again, the parts exactly as they were read and then the orchestra: each converter's part,
instrument, stops, octave, volume and place, and the room and the onset.
Read that tape back in later and everything is set as you left it.

Samson's own player, *pdp1m13*, still reads such a tape's parts; it halts at the orchestra block after
the last of them, since it doesn't know what that block is.
The 1.0 player reads a tape punched by this one as far as its orchestra, says TAPE ERROR BAD COUNT there, and
keeps and plays its parts.
A tape with dynamics is for this player only.
*pdp1m13* halts at its first dynamic, and the 1.0 player doesn't know them.

### The messages

| Status line | Meaning |
|---|---|
| READING TAPE, TAPE READ | A READ TAPE under way, and done |
| NO TAPE | No word came from the reader in about 12 seconds. The parts already loaded are kept |
| TAPE ERROR BAD COUNT, TOO MANY PARTS, TAPE TOO LARGE | The read stopped. The parts read whole before it are kept. More than 30 parts, or a tape too large for core with the program, is too large; the whole Haffner fits |
| TAPE READ CHECKSUM ERROR | A block's checksum failed. It is taken as read, as Samson's player takes it |
| TAPE READ ORCHESTRA REFUSED | The tape's orchestra names a voice the drum doesn't have, or a volume, place, room or onset the menu can't show. The orchestra is unchanged |
| PUNCHING TAPE, TAPE PUNCHED | A PUNCH under way, and done |
| LOADING VOICES | PLAY is reading the orchestra's voices from the drum |
| NO VOICES ON THE DRUM | The drum doesn't hold a voice library this player reads, see *The drum* |
| VOICE NOT IN THE LIBRARY | The orchestra names a voice the drum doesn't have |
| VOICE n DAMAGED, RELOAD THE DRUM | A block of voice n failed its checksum. Copy the drum file in again |
| VOICES TOO LARGE | The orchestra's voices don't fit in core together. Choose smaller ones |
| VOICES DO NOT FIT WITH THIS TAPE | The tape is over 7,424 words and the orchestra's voices don't fit in the core it leaves. Choose smaller ones, or play a smaller tape, a movement for instance |
| STOPS DO NOT MATCH | A converter's stops are from different divisions, or a stop is mixed with another voice |
| STOPPED | STOP ended the piece |

## The tapes

| Tape | What | Drum |
|---|---|---|
| *tapes/littlefugue-biggs.bin* | Bach's Little Fugue, BWV 578, on the Flentrop in Biggs's registration: the Hoofdwerk plenum on the two upper parts, the Pedaal on the bass, coupled to the Hoofdwerk | Flentrop |
| *tapes/wachetauf-biggs.bin* | *Wachet auf*, BWV 645, at the tempo of Biggs's recording: the ritornello on the Hoofdwerk flutes, the chorale on the Kromhoorn, the pedal on the Gedekt | Flentrop |
| *tapes/toccata-biggs.bin* | The Toccata of BWV 565, to the fugue's entry, at the tempo of Biggs's recording, on the Hoofdwerk plenum and the Pedaal | Flentrop |
| *tapes/sonata1-stereo.bin* | Samson's Trio Sonata I on the default organ, spread across the stereo field | Any of the three with the nine |
| *tapes/scotland-brave.bin* | *Scotland the Brave* on the Highland bagpipe, the chanter in the center and the drones to the left | Bagpipe |
| *tapes/amazing-grace-piano.bin* | *Amazing Grace* on the recorded piano, with no room | Recorded-2 |
| *tapes/haffner-1968.bin* | Mozart's Haffner Symphony, K. 385, whole, with its dynamics and the fourth movement's trills, from the 1968 source in */opt/pidp1-mods/tapes/old/mozart_sym35_1feb68.bin*, on the principal, gedackt, flute and bourdon at 4 feet. For this player only, see *Punch it* | Any of the three with the nine |

Each tape carries its room and onset, so it plays as intended whatever *pidp1.config* says.
The four organ tapes and the Haffner have VOICES and 8 MS, the bagpipe VOICES with the onset off, and
*Amazing Grace* no room and no onset.

The two tempo tapes take only their timing from Biggs's recordings, by matching a rendering against the record,
no music from the recordings is in them (copyrighted).
The Toccata's notes are an arrangement for four parts, both hands sharing three.

All the original sources for the tapes were, to the best of my knowledge, public domain.

Samson's Harmony Compiler tapes in */opt/pidp1-mods/tapes*, the trio sonatas I, III and IV, play too on the
default organ or whatever you set.
*trioSonataIII_intermediate.bin* has a damaged bar pointer in its ninth part. READ TAPE says TAPE READ CHECKSUM
ERROR, and the bass of the third movement falls silent at bar 71.
That is the tape, not the player.

## The voices

### The nine basic voices

| Family | Voices |
|---|---|
| ORGAN | principal, gedackt, bourdon |
| WINDS | flute, reed, trumpet |
| PLUCKED | plucked bass, harpsichord |
| PERCUSSION | marimba |

On the standard and Hammond drums all nine are recorded.
The organ voices are pipes of the organ of Bureå Church in Sweden.
The principal is an open 8-foot pipe, the gedackt a stopped one, the trumpet the organ's trumpet stop, a reed,
and the bourdon its 16-foot pedal Subbas to F3 with a stopped 8-foot rank above.
The flute, the reed (an oboe) and the plucked bass (a double bass, pizzicato) are orchestral recordings, the
marimba a marimba struck hard.
Each is 5,400 to 9,600 words, so any four of the first eight fit in core together.
The harpsichord is larger, see *The recorded instruments* below.

The Flentrop drum keeps the synthesized nine, built from harmonics and envelopes, the organ pipes with a chiff
as they speak.
There is no room for the recorded harpsichord beside the stops, and the Flentrop keeps its sound.

### The Flentrop

The 27 stops of the 1958 Flentrop organ in Adolphus Busch Hall, Harvard, the organ of E. Power Biggs's famous
recordings with a family for each division.

| Division | Stops |
|---|---|
| RUGPOSITIEF | Holpijp 8, Prestant 4, Roerfluit 4, Gemshoorn 2, Quint 1 1/3, Mixtuur II, Kromhoorn 8 |
| HOOFDWERK | Prestant 8, Roerfluit 8, Octaaf 4, Speelfluit 4, Nasard 2 2/3, Valkfluit 2, Terts 1 3/5, Mixtuur IV |
| BORSTWERK | Zingend Gedekt 8, Koppelfluit 4, Prestant 2, Sifflet 1, Regaal 8 |
| PEDAAL | Bourdon 16, Prestant 8, Gedekt 8, Fluit 4, Mixtuur III, Fagot 16, Trompet 8 |

The Pedaal is a 16-foot division so a pedal part at 8 FT sounds at 16 feet as on the organ.
The mixture make-up is a usual Dutch one; the Flentrop's own isn't documented.

The stops are voiced as a tracker organ speaks.
Each note has the division's chiff, a large pipe speaks more
slowly than a small one with a breath of wind as it does, the flutes on the fundamental have the edge tone
of the wind at the pipe's mouth, and each converter has a slight céleste, a second rank 2 cents sharp that beats
slowly with the first.

### The recorded instruments

These were cut from actual recordings into voices, the violin, string section, trumpet,
French horn, piano, guitar and a drum kit.
Each is the real instrument's sound, a recording every few semitones, with the vibrato on the violin.

The drum kit sits where General MIDI puts its drums: kick C2, snare D2, floor tom F2, closed hi-hat F#2,
low tom A2, open hi-hat A#2, high tom C3, crash C#3, ride D#3, ride bell F3.
Every other pitch is silent.

A recorded voice is large, 13,000 to 38,000 words, and the voices of an orchestra must fit in core together,
banks 5 to 17 octal, 45,056 words.
That is room for two of them, and sometimes three.
PLAY says VOICES TOO LARGE when they don't fit.
A converter with no part still loads voice 0 which is why each recorded drum starts with the small flute,
the synthesized one, 1,536 words.

The bagpipe's chanter is recorded too, each of its nine notes a zone, tuned to A 440 over the drones.

So is the harpsichord, voice 8 of the standard and Hammond drums.
It is an English harpsichord's 8-foot register, a recording every whole tone from A#1 to E6, a zone every six
semitones, with each note's decay and the pluck.
At 38,010 words it is the largest voice.
Beside it an orchestra has room for the flute, and for the plucked bass when every converter has a part, since
a converter without one loads the flute too.
With any other voice of the nine it is VOICES TOO LARGE.
A tape larger than 7,424 words leaves less room still.
*midi2dac* maps the General MIDI pianos and guitars to the harpsichord too, so a MIDI file mixing them with organs
can be refused the same way.

### The Hammond

Not recorded, built.
This is a Hammond B3 as a stop of sine tonewheels, the drawbars its ranks, with a key click and a Leslie.
The recorded-2 drum has two of them, *B3 slow* and *B3 fast*.

The Hammond drum has 42 presets, 12 registrations under each Leslie speed, families LESLIE SLOW, LESLIE FAST and
LESLIE STOP and two with percussion, PERC2 and PERC3, each at the three speeds.
They are stops, so a converter can sound up to four of one family.
*FULL ORGAN* with *FULL 1FT* is the full organ with its 1-foot drawbar.

### The octave a note sounds at

A part at 4 FT or 2 FT sounds up to C#7 on most voices and to B6 on the sampled and recorded ones and the
Terts.
A note that would go higher drops back an octave, as an organ's high ranks do.

## Loudness

Each channel is the four converters added and halved, so two converters on one side can never pass full scale.
Three or four on one side, or two centered with two more off-center, can, on a loud passage, clip.
The answer is the sliders.
One step, 3 dB, down on each centered pair, and more for three on a side.
*sonata1-stereo.bin* is set up that way.

The room adds its own sound so with a room the whole sound is turned down a little to make space for it,
6 dB in the large hall and 3 dB in Busch Hall.

**Dynamics.** A tape the Harmony Compiler punches under LANGUAGE 1968 DAC carries the score's dynamics, see
*/opt/pidp1-mods/Docs/UsingHarmony.md*.
Each converter's slider sets its level at f, and each dynamic moves the level from there.
pp is 18 dB softer, p 12, mp 9 and mf 6, and ff is 3 dB louder, never past the slider's span.
Every movement starts at f.
A slider moved while such a tape plays moves the whole line, dynamics and all.

*dacgain* scales the output, 1.0 is as the tapes were made.
This normally shouldn't need to be changed.

When the PDP-1 stops, the IOT prints a line on the emulator's standard error:
```
IOT 71: late samples 0 0 0 0, lost fetches 0, clipped frames 0, windows 0
```
Clipped frames above 0 means turn something down.
Late samples above 0 means the channel couldn't keep up, which none of the voices here should cause.

## The room

The sound can be heard in a room as it would be from a real recording.
Two are provided, Adolphus Busch Hall where the Flentrop is located or a generic concert hall.

| Setting | Room |
|---|---|
| dacroom=voices | Busch Hall when the voices playing ask for it, as the Flentrop's stops do, the hall otherwise |
| dacroom=busch | Always Busch Hall |
| dacroom=hall | Always the hall |
| dacroom=none | No room, the converters' sound as it is |

*voices* is the one to use.
The organ gets its own hall and everything else a hall that suits it.

The room is not implemented in the IOTs, it is in the waveforms.
It takes no machine cycles.

*dacroom* is only where the converters start.
A program changes the room with *dye* (*Implementation.md*), and the player does from a tape or its ROOM field.

## The onset

On a tracker organ the notes of a chord don't all speak at the same instant.
*daconset=8* starts each organ note up to 8 ms late, from a fixed sequence for each converter, so a tape plays
the same every time.
The timing of the piece doesn't move, only where each note begins to sound.
Set it to 0, or leave it out, for every note exactly on time.
As with the room, a program can change it, with *dyo*, and the player does from a tape or its ONSET field.

## A MIDI file to a tape

*midi/midi2dac* makes a tape from one or more MIDI files, each file a movement, with no PDP-1 and no Harmony
Compiler.
In */opt/pidp1-mods/FunStuff/Dacmusic*:
```
midi/midi2dac ~/amazing-grace.mid -o ~/grace.bin
```
The player has four converters, one note at a time each, so a piece gets four parts at most.
*midi2dac* shares them among the file's tracks, splits a track that plays chords, and says what it had to leave
out, by part and bar.
A file played in, rather than written in a notation program, needs *--played*.
Its notes are then snapped to 64ths.
The tape has the file's tempo and its dynamics, from the notes' velocities.

READ TAPE it, set PARTS A MOVEMENT to the number *midi2dac* gives, set the orchestra and PLAY.
Or give the tape its orchestra with *--orchestra* and a short text file naming each converter's part, voices,
octave, volume and place, and the room and the onset; then the tape sets them all itself.
*--plain* makes a tape for *pdp1m13* and the 1.0 player.
*midi/README.md* has the details.

## Rendering a tape to a wav file

*render/dacrender* plays a tape into a 50 kHz stereo wav file, exactly as the device plays it, with no PDP-1, no
emulator and nothing else needed.
It's how to share the music.

In */opt/pidp1-mods/FunStuff/Dacmusic*:
```
render/dacrender tapes/sonata1-stereo.bin sonata1.wav --room=voices
render/dacrender tapes/littlefugue-biggs.bin fugue.wav --voices=voices/lib/flentrop.lib --room=voices --onset=8
```
The nine basic voices are built in.
For other voices name the library with *--voices=*, the same library the drum was made from:
*voices/lib/flentrop.lib*, *instruments/hammond.lib*, *instruments/recorded-1.lib* or *instruments/recorded-2.lib*.
*--secs=S* renders just the first S seconds.
A tape that carries a room and onset is rendered in them; *--room=* (*voices*, *busch*, *hall* or *none*) and
*--onset=MS* override the tape.

A whole trio sonata takes a few seconds.
*render/README.md* has the details, and how to make an MP3.

## Making your own voices

A voice is a text file.
**dacvoice** makes voices from a short definition, its harmonics, its envelope in seconds and dB, its timbres, or
for an organ stop, its ranks.
**dacdrum** puts a library, a list of voice files, on a drum.
```
voices/dacvoice voices/defs/clarinet.def clarinet.voice
voices/dacdrum mylibrary.lib drums/pdp23drum-mine
```
Keep the nine basic ones first in your library so every tape still names the same voices,
or at least keep a small voice first, since voice 0 is always loaded, such as *voices/lib/flute-synth.voice*,
1,536 words.
The synthesized eight are there too, *voices/lib/\*-synth.voice*.
*voices/README.md* describes the definitions, the voice files and the drum.

## Programming the converters yourself

The converters are an ordinary device with ordinary IOTs, and *dacdefs.ah* defines them for am1.
At its simplest a converter plays a buffer of samples, over and over, at a period you choose.
At its most it plays a sampled note at any pitch through an envelope, with vibrato, in stereo.
*dye*, *dyo* and *dys* set and read the room and the onset for the whole device.
*Implementation.md* describes every command and gives an example.

## Credits

- **Peter Samson** wrote the Harmony Compiler and its player, whose tape format and timing this player keeps.
  His trio sonata tapes are in */opt/pidp1-mods/tapes*.
- **J. S. Bach** wrote the music.
- **E. Power Biggs**: the tempo of *wachetauf-biggs.bin* and *toccata-biggs.bin* is measured from his recordings on
  the Flentrop in Busch Hall, and the registrations follow his.
- **Wachet auf, BWV 645**: the notes are from the Mutopia Project's piece 601, typeset from the Bach-Gesellschaft
  edition, licensed Creative Commons Attribution-ShareAlike 2.5.
  *wachetauf-biggs.bin*, and anything rendered from it, is an adaptation and carries the same license,
  https://creativecommons.org/licenses/by-sa/2.5/
- **Toccata, BWV 565**: the notes are from the Mutopia Project's piece 1780, from the Bach-Gesellschaft
  Ausgabe, public domain.
- **The recorded instruments**: violin, trumpet, horn, piano and guitar are cut from the University of Iowa
  Electronic Music Studios' Musical Instrument Samples, free to use, https://theremin.music.uiowa.edu/MIS.html.
  The string section is from Versilian Studios' VSCO 2 Community Edition, CC0,
  https://versilian-studios.com/vsco-community/, and the harpsichord from their Versilian Community Sample
  Library, its English harpsichord (a Zuckermann kit), CC0, https://github.com/sgossner/VCSL.
  The bagpipe chanter is from IowaSpaceWizard's *Great Highland bagpipe samples* on Freesound, CC0,
  https://freesound.org/people/IowaSpaceWizard/packs/29644/.
  Thanks to all three.
- **The basic nine**: the flute, reed (an oboe) and plucked bass (a double bass, pizzicato) are cut from
  VSCO 2 Community Edition, and the marimba from the Versilian Community Sample Library, both CC0, as above.
  The principal, gedackt, trumpet and bourdon are cut from **Lars Palo** and **Graham Goode**'s sample set of
  **Bureå Church**, Sweden, https://familjenpalo.se/vpo/about-burea-church/, licensed Creative Commons
  Attribution-ShareAlike 2.5 Sweden, https://creativecommons.org/licenses/by-sa/2.5/se/.
  The samples were changed.
  They were mixed to one channel, resampled, cut and looped.
  Those four voice files, and the standard and Hammond drums holding them, carry the same license;
  its text is *CC-BY-SA-2.5-SE.txt*, here beside this file.
- **The drum kit** is cut from *MuldjordKit by Lars Muldjord (DrumGizmo)*, through its SFZ port by kinwie,
  licensed CC BY 4.0, https://creativecommons.org/licenses/by/4.0/.
  The samples were changed.
  They were mixed to one channel, resampled, cut and looped.
  The kit voice, the drum holding it, and anything rendered from it carry this credit.

Enjoy!\
Bill Ezell
