# How the D/A converters and the music player work

This document describes the implementation of the DAC code: IOTs 71 and 72, the music player, the voices,
and the renderer.

See *UsingDAC.md* for how to actually use the programs.

This is version 1.0\
Edit date 13-Oct-2026

## The pieces

| Piece | Where | What it does |
|---|---|---|
| The device | *IOTs/DAC/IOT_71.c* | Four converters, their channel, mixer, room and audio output |
| IOT 72 | *IOTs/DAC/IOT_72.c* | Only an alias, its commands are handled by IOT 71's plugin |
| The room | *IOTs/DAC/room.h* | Busch Hall and the generic hall, in integers |
| The voicing | *IOTs/DAC/edge.h* | The edge tone, the speech and its breath, the celeste's detunes, in integers |
| The mnemonics | *Am1Includes/DAC/dacdefs.ah* | Every command for every converter, with its bits |
| The player | *music/dacmusic.am1*, *menu.ac*, *menus.ac* | Reads a tape, loads voices from the drum, feeds the converters, draws the menu |
| The voices | *voices/*, *instruments/* | Voice files, libraries, and the tools that make them and write drums |
| The renderer | *render/dacrender.c* | The device and the player, on a host, tape in and wav out |

*room.h* and *edge.h* are compiled into both the device and the renderer, so both compute the same integers.
During development everything here was checked against a model in Python, word for word and block for block;
the model and the tests are not part of the release.

## The device

### The idea

Four 12-bit D/A converters, each fetching its own samples from core through high speed channel 2, with a
multiplexer in front of the channel.
Channel 1 is the Type 23 drum's and channel 3 the Type 340's.
Every sample a converter fetches is one word through the channel, and so steals exactly one machine cycle;
when two converters want a word in the same cycle the lower numbered goes first.
A word reaches its converter the cycle after it was due.
If a word is still waiting for the channel when the next one falls due, it is replaced and counted late.

A sample is bits 0-11 of the word, a ones' complement number; the low 6 bits are ignored, so a program can keep
full 18-bit values in its tables.

The device has six layers, each switched on by the program, so a program that uses only the first ones never
meets the rest:
1. **Buffer mode**: play a buffer of samples at a fixed period
2. **Wavetable mode**: play a table at any pitch through an envelope, timed by an event counter
3. **Sample mode**: play a sampled note, attack once then a loop, with timbres that follow its level
4. **The volume and the place**: a level and a position in the stereo field for each converter
5. **The voicing**: an organ pipe's edge tone, speech and celeste
6. **Recorded instruments**: interpolation, packing, a slow counter for vibrato and Leslie, and percussion

### The instructions

Instruction bits 6-9 are the command and bits 10-11 the converter, 0-3.
The mnemonics number the converters 1-4: converter N's instruction is converter 1's plus (N-1) x 0100.

Bit 6 is also the CPU's completion request, so the commands with it set, IOT 71's 8-15 and IOT 72's 010-017, give
no completion.
One given would set the CPU's ios flag, and a later *ioh* meant for some other device would then pass without
waiting.
The commands without bit 6 complete at once.
It does *not* support completion, i or c.
If you use either, your program will hang.

**IOT 71, buffer mode**

| Mnemonic | Converter 1 | IO | What it does |
|---|---|---|---|
| dxaN | 720071 | bits 2-17 | Latch the next buffer's address, a full 16-bit address |
| dxlN | 720471 | bits 6-17 | Latch its length in words, 0 = 4096 |
| dxpN | 721071 | bits 6-17 | Latch its sample period in machine cycles, 0 = 4096 |
| dxgN | 721471 | | Load every latch and start; the first word is fetched at once |
| dxhN | 722071 | | Halt; the output falls to 0 |
| dxsN | 722471 | | Skip if the flag is set, and clear it |
| dxbN | 723071 | bit 0, bits 14-17 | Bit 0 set: a sequence break at each new buffer on the channel in bits 14-17 |
| dxtN | 723471 | returned | Bit 0 running, bit 1 the flag, bits 5-17 the words left in this buffer |

The address, length and period are latched.
*dxg* loads them, and so does the start of every later buffer, at the sample time its first word is due.
That same moment sets the flag.
So the flag means a new buffer has begun, and the program has the whole of that buffer's time to latch the one
after it.
A buffer repeats until the program latches another.

**IOT 72, wavetable mode**

| Mnemonic | Converter 1 | IO | Latches |
|---|---|---|---|
| dyfN | 720072 | 18 bits | The step, 6 integer and 12 fraction bits, in table words a sample |
| dycN | 720472 | 18 bits | The event's length in machine cycles, 0 = 262,144 |
| dyaN | 721072 | 18 bits | The attack rate |
| dydN | 721472 | 18 bits | The decay rate |
| dyrN | 722072 | 18 bits | The release rate |
| dylN | 722472 | bits 0-8, 9-17 | The start level and the peak level, 1/8 dB of attenuation |
| dyxN | 723072 | bits 0-8, 11-17 | The sustain level (0777, none) and the actions |
| dytN | 723472 | returned | Bits 0-2 the envelope's stage, bits 6-17 the attenuation in 1/64 dB |
| dyqN | 724072 | | Commit every latch to the queue, see below |
| dyvN | 724472 | 18 bits | The slow counter's rate |
| dywN | 725072 | bits 0-8, 9-16, 17 | The vibrato's depth, its delay, free running |
| dymN | 725472 | bits 0-8, 9-17 | The Leslie, the tremolo's depth and the places, 1/8 dB |
| dykN | 726072 | bits 0-8, 15-17 | The percussion's level, 1/8 dB under the note, and its multiple of the step |
| dyjN | 726472 | 18 bits | The percussion's decay rate |
| dypN | 727072 | bits 2-17 | The address of the percussion's sine, 256 words |

The actions, in *dyx*'s low bits:

| Action | Meaning | Kind |
|---|---|---|
| 01 | Wavetable mode; clear for buffer mode | Held |
| 02 | Key on | Once |
| 04 | Key off | Once |
| 010 | Phase to 0 | Once |
| 020 | Sample mode, with 01 | Held |
| 040 | Interpolation | Held |
| 0100 | Packing, with 020 | Held |

A held action stays as it is until a *dyx* changes it; a one-shot applies at the next load and is consumed.

**IOT 71's commands 8-15, sample mode, the volume and the place**

| Mnemonic | Converter 1 | IO | What it does |
|---|---|---|---|
| dzeN | 724071 | bits 2-17 | Latch the sample's end, 0 = 65,536 |
| dzlN | 724471 | bits 2-17 | Latch the loop's length |
| dzsN | 725071 | bits 2-17 | Latch the timbres' stride, 0 = one timbre |
| dzuN | 725471 | bits 0-8, 9-17 | Latch thresholds 1 and 2, 1/8 dB, 0777 = none |
| dzvN | 726071 | bits 0-8, 9-17 | Latch threshold 3 and the voicing |
| dzgN | 726471 | bits 9-17 | The volume, 1/8 dB of attenuation, 0777 = mute; not latched |
| dzpN | 727071 | bits 0-8, 9-17 | The place: the left and right attenuations, 1/8 dB, 0777 = off; not latched |
| dztN | 727471 | returned | Bits 2-17 the phase's integer part |

The end, loop and stride are in words, or in groups of three samples for a packed voice, below.

Every latch, IOT 71's buffer latches included, loads together at the end of the event playing, or at *dxg*.
*dzg* and *dzp* are not latches, they act at once as a knob does, see *The volume and the place*.

**IOT 72's command 017, the room and the onset**

These are the device's own settings, one of each for all four converters, so the converter field picks the
command instead of a converter.

| Mnemonic | Instruction | IO | What it does |
|---|---|---|---|
| dye | 727472 | bit 0, bits 16-17 | The room: bit 0 set, the configuration's; else 0 none, 1 the hall, 2 Busch Hall, 3 the voices' choice |
| dyo | 727572 | bit 0, bits 9-17 | The onset's bound: bit 0 set, the configuration's; else in 1/8 ms (25 cycles), 0 off |
| dys | 727672 | returned | Bits 0-2 the room set, as *dye*'s, bits 3-5 the room playing (0 none, 1 the hall, 2 Busch Hall), bits 9-17 the onset's bound in 1/8 ms, 0777 if longer |

*dacroom* and *daconset* set both when the emulator starts.
A setting given holds until another is given or the emulator restarts, through a halt and a new program.
Neither is a latch: the room changes at once and starts empty, the onset applies from the next event's end, and a
note it holds loads as it was going to.
The fourth field, 727772, does nothing.

### Wavetable mode

*dxa* latches the table's address and *dxl* its length, a power of two up to 4096; *dxp* latches the sample
period N, as in buffer mode.

The phase is 12 integer and 12 fraction bits.
At each sample time the converter fetches the word at the address plus the phase's integer part, masked with
the length - 1, then adds the step.
So the pitch is step x 200,000 / (N x length x 4096) Hz, with the step in its raw 1/4096 units.

**The envelope.** The attenuation is 12 integer bits of 1/64 dB and 12 fraction bits.
Every machine cycle its stage adds its rate or subtracts it:
- **Key on**: the attenuation jumps to the start level, then ATTACK falls to the peak level
- **DECAY** rises to the sustain level; with no sustain level, 0777, it runs on to the floor
- **SUSTAIN** holds
- **Key off**: RELEASE rises to the floor
- **OFF**, at the floor, 63.99 dB: the output is 0 and the converter fetches nothing, so a rest costs no cycles

A rate unit is 1/262,144 dB a cycle, 0.763 dB a second.
A rate of 0 holds its stage.
The levels are 9 bits of 1/8 dB.

The gain is 10^(-a / 1280), *a* the attenuation's integer part: an exponential converter driving the multiplying
converter's reference, so the output follows the gain every cycle, between samples too.

A key on drops the held sample and any word of the old note still on its way.
Until the new note's first word arrives the output is 0.

**The event timer.** Each converter counts its event down in machine cycles.
At its end every latch loads, the actions apply, and the flag sets with a sequence break if *dxb* asked for one.
As in buffer mode the flag means a new event has begun and the program has the whole event to latch the next.
A latch keeps its value so an event with no new length repeats its length.

**The queue.** Behind the latches each converter has one more set, the queue.
*dyq* commits the latches as they stand, the event length included, to the queue and the one-shot actions are
consumed from the latches as a load consumes them.
At an event's end, and at *dxg*, a full queue loads.
An empty one leaves the latches as they are, like unqueued does.

If the queue is full the commit waits and the latches go to the queue at the next event's end after it has
loaded.
*dxh* or a halt empty the queue.

A program that keeps two entries ahead therefore has two events after each flag to commit the next, not one.

### Sample mode and the followed timbre

Sample mode, action 020, plays a sampled note, an attack once, then a loop.
The phase's integer part is 16 bits and is not masked so a sample can be up to 65,535 words.
After each step a phase at or past the end has the loop's length subtracted from it, fraction kept, so the loop
is seamless at any step.
The loop must be at least 64 words.

A key on with the phase reset starts the attack again,
A slur, a new step with no key on, carries on in the attack or the loop.

**The timbre that follows the level.** Up to four timbres stored one after another at the stride and three
thresholds in the envelope's levels.
At each sample the timbre is the number of thresholds the attenuation has reached and the fetch is from the
address plus timbre x stride plus the phase.
The timbres share their harmonics' phases.
This is how a note grows darker as it fades, as a struck or plucked note does, with no work by the program.
A stride of 0 turns it off.
It works in both modes.

### The volume and the place

**The volume** is an attenuation added to the envelope's before the exponential converter.
*dzg* is not latched, the volume heads for its new value at once at 1/64 dB a cycle, an up/down counter, so 3 dB
takes about 1 ms.
The thresholds compare the envelope's attenuation alone so a part keeps its tone.
It starts at 0 dB, a gain of exactly 1, and nothing but *dzg* changes it.

**The place.** After the four outputs is an analog mixer.
Each converter reaches each channel through a multiplying converter whose reference
an exponential converter sets, same as for the volume.

*dzp* gives both attenuations which slew at 1/64 dB a cycle like the volume.
At power on converters 1 and 2 are at 0 dB left and off right, 3 and 4 the other way, so a program that never
gives *dzp* hears the original mix.
The player uses constant power over nine places.

Each channel is the four converters with their gains, summed in converter order and halved.
With gains of exactly 1 and 0 the extra terms add exact zeros so the original mix is bit for bit what it was.

### The voicing

*dzv*'s bits 9-17 carry a voice's voicing, latched and loaded with the note.
It is for the organ stops, see *edge.h**.

| Bits | What |
|---|---|
| 15-17 | The edge tone's level k: 0 off, else -36 + 2k dB under the stop at C4 |
| 14 | The speech |
| 13 | Busch Hall, for *dacroom=voices* |
| 9-12 | The celeste's detune in half cents, 0 none |

- **The noise**: each converter has a 32-bit xor-shift generator seeded at its *dxg* from its number, stepped every
  fourth cycle, 50 kHz, while the note has the edge or the speech.
- **The edge tone**: the noise through a state-variable band-pass at the note's fundamental, quality 3, under
  the note's envelope, 4 dB softer each octave below C4.
  It is the wind at a flue pipe's mouth.
- **The speech**: at a key on, the attack is stretched to take 10 of the note's periods unless the voice's own
  attack is slower so a large pipe speaks more slowly than a small one.
  Through the rise comes a breath, the noise through a band-pass at twice the fundamental, 24 dB under the
  stop, dying away over the rise.
- **The celeste**: a second phase accumulator steps k half cents sharp of the first, and at every other sample
  time it fetches the same table a second time.
  The two words are summed at equal power, a rank a little off the unison beating slowly with the first.
  Each second read steals one more cycle.

The band-pass coefficients come from the step and the period by a Taylor series, and the levels from an integer
log, so there is no *sin()* or *pow()* in it and every host gets the same integers.

### Recorded instruments

Four more abilities, each the voice's choice and off at 0.

**The interpolation**, action 040.
Each read holds its last two words, each with its address.
At a sample time it needs the word at the phase and the next one.
A word it already holds is kept, any other is fetched.
One cycle later, with both in, the sample is formed as s0 + (((s1 - s0) x f + 2048) >> 12), *f* the phase's
fraction: a straight line between the two samples, on a shift-and-add multiplier the four converters share.
Since words are only fetched when the phase moves past them, the steals follow the rate the samples are stored
at, not N.
That lets a recording be stored at just the rate its notes need.

**The packing**, action 0100 with sample mode: three 12-bit samples in each two words.

| Sample | Bits |
|---|---|
| 3g | Word 2g, bits 0-11 |
| 3g + 1 | Word 2g, bits 12-17, then word 2g + 1, bits 0-5 |
| 3g + 2 | Word 2g + 1, bits 6-17 |

The end, the loop and the stride are then in groups.
The attack, the loop and each timbre always start on a group.
The phase stays in samples, and its index divided by three gives the group and the place, by a constant, a
residue chain in 1960s logic.
Any sample and the next are always within two words, a read never needs more than its pair.
A packed voice fetches two thirds of the words, and core and the drum hold half again as many samples.

**The slow counter**: a 24-bit phase clocked every 64th cycle, 3,125 Hz, its rate from *dyv*.
Two triangle waves a quarter turn apart are read from it:
- **Vibrato**: the step moves by up to the depth, 0.106 cents a unit, so 53 cents at 0777.
  A key on sets the counter to 0 and holds it there for the delay, so the pitch leaves the note's own smoothly.
- **Free running**, *dyw* bit 17: a key on leaves the counter alone, so it runs on from note to note, a rotor.
- **The Leslie**: the tremolo adds up to its depth to the attenuation, and the place's depth swings the two place
  attenuations in opposite directions, a quarter turn from the tremolo, as a Leslie horn's level and position
  move.

**The percussion**: a third read of a 256-word sine at a multiple of the note's step that dies away.
It starts at a key on that finds the envelope OFF or in RELEASE, a detached note, as a Hammond's single
trigger does, and decays at its own rate through the note's gain and its own.

### The channel and the window timer

Each converter has six slots on the channel, served in order: the first read's two words, the celeste's two, the
percussion's two.
A voice without interpolation uses just one slot for each of its reads.

A channel break granted in an instruction's cycle 0, or in its defer cycle starts the instruction again
(PDP-1 Maintenance Manual F-17, 6-18).
A deferred instruction needs three free cycles in a row: its cycle 0, its defer, and the cycle a request
waits for its steal to complete.
Four converters at a period of 10 can, at some start phases, leave no such window for as long as their notes
last, and the program then halts at its next deferred instruction.

The multiplexer counts cycles.
Once 40 have passed without three free in a row, it starts no read until there have been three, and the reads
due meanwhile wait in their usual order.
The program then completes a deferred instruction at least every 43 cycles.
Under a load that leaves windows of its own the timer never acts and the timing is exact to the cycle.
It sees only this channel, the drum's and the 340's steals can still fall in a window.

### The onset

With *daconset=MS*, or *dyo* giving MS, at an event's end the event timer reloads and the flag sets exactly as without it, but the
note the end loads is taken then and applied d cycles later.
d is drawn from 0 to MS milliseconds, never more than half the new event, from each converter's own xorshift
sequence, seeded at *dxg*, so a tape plays the same on every run.
With no onset at *dxg* every converter's first note starts together.
A note is never half loaded.
It costs no instruction and moves no flag, the program's timing is unchanged.

### The order within a cycle

The plugin's *iotIOPoll()* runs once a machine cycle, on the emulator's thread:
1. The word on the channel is collected and delivered
2. Each running converter's volume and place step toward their targets
3. Each converter in wavetable mode samples whose words have landed are formed, the envelope steps, the
   percussion decays, the breath dies away, a note the onset holds loads, the event timer counts, the sample
   clock counts and marks its reads due, every fourth cycle the noise steps, every 64th the slow counter
   clocks, and the output is computed
4. A converter in buffer mode counts its period, starting a new buffer when one ends
5. The next due read goes on the channel unless the window timer holds it
6. The outputs are mixed

### The mixer, the room and the audio

Each output frame is the mix averaged over 4 machine cycles so the stream is 50,000 frames a second of machine
time.
*dacgain* scales it.

**The room**, *room.h*, comes after the mixer, so every converter, place, and volume reaches it as a listener
would hear them.
- **Busch Hall**: a 40 ms predelay, two diffusers, then eight delay lines of 50 to 112 ms in a Householder
  feedback network.
  Each line is split into three bands by two one-pole low-passes, at 250 Hz and 4 kHz, each band with its own
  decay, so the hall falls 60 dB in 4.1 s at 100 Hz, 3.4 s at 1 kHz and 1.9 s at 10 kHz.
  Beside it, fed the same predelayed sound, is Dattorro's plate reverberator, 1.8 s.
- **The hall**: the same eight lines with one decay, 3.0 s, a gentle loss of highs, and a 1/60 s predelay.

Everything is integers, delay words of 32 bits, sums of 64, coefficients in 1/65,536 rounded by adding 2^15 and
shifting 16.
Every coefficient is computed once and is written as a number in *room.h*; nothing calls *exp()* or *pow()*,
so a Raspberry Pi and an x64 agree to the bit.
With a room the whole sound, dry and wet, is cut by the room's trim, 3 dB in Busch Hall and 6 dB in the hall,
sized by the loudest tapes of the voices each room is for.

With *dacroom=voices*, or *dye* giving 3, the room is chosen at each *dxg*, Busch Hall if a running converter's
note has bit 13 of its voicing, the hall otherwise.
A room changed starts empty.
*dye* changes the room for the rest of the run, and the device logs it, `IOT 71: room hall, set by the program`;
*dyo* likewise, `IOT 71: onset 1600 cycles, set by the program`.

A frame past full scale is clipped and counted.
Frames go to SDL, version 2 or 3 as the emulator is built, in batches of 250 to mimimize jitter.

### Starting and stopping

The plugin sets itself up the first time a program gives IOT 71 or 72.
It claims channel 2, reads its three settings, opens the audio device and logs them:
```
IOT 71: gain 1.000
IOT 71: room from the voices, hall until one chooses busch
IOT 71: onset 1600 cycles
IOT 71: 50000 Hz stream
```
When the pidp1 stops, by a halt or the STOP switch, the converters stop with it, their queues
empty, the audio stops, and it prints its counts:
```
IOT 71: late samples 0 0 0 0, lost fetches 0, clipped frames 0, windows 0
```
Late samples are per converter, words replaced while still waiting for the channel.
Lost fetches are requests the channel refused.
Those two count from the emulator's start, the last two from the last stop.
Clipped frames are output frames with a channel past full scale.
Windows is how many times the window timer held a read.

### An example

A converter in buffer mode playing a table of samples, over and over:
```
#include <DAC/dacdefs.ah>

    lio [tone           // the buffer, a full 16-bit address
    dxa1
    lio [0d100          // 100 words
    dxl1
    lio [0d10           // a sample every 10 cycles, 20,000 a second
    dxp1
    dxg1                // start; the buffer repeats until another is latched
wait, dxs1              // skip once the next pass of the buffer has begun
    jmp wait
```
With one period of a wave in *tone*, that is a 200 Hz tone.
To play something else after the current buffer, latch its address and length after a *dxs* skips.
It starts when the current one ends with no gap.

## The player

*dacmusic.am1* plays Samson's Harmony Compiler intermediate tapes, the same tapes his *pdp1m13* plays<
so his notation, his compiler, and his scores stay as they are.

### Memory

| Where | What |
|---|---|
| Bank 0 | The program, the menu's display list and their data |
| Bank 1 | The drum's directory, a 190-word slot for each converter's voice header, the voice loader and the menu's lists |
| Banks 2-3 | The tape from 020000 to 037277, 7,872 words, each part with its counts and checksums kept for PUNCH |
| Bank 3 from 037300 | PUNCH and the orchestra block it builds, and the places' tables |
| Bank 4 | Each converter's ring, 256 entries of 4 words |
| Banks 5-15 | The voices the orchestra names, from 050000 |

The extend-mode window is open throughout, every indirect address is a full 16-bit one.
The Type 340's subroutine instructions reach banks 0 and 1 only, so the menu's texts are there.

### Reading a tape

A part is two blocks, each a count, its words, and a checksum: the notes, then the bar pointers, the order the bars
are played in.
A note word holds the pitch (2 is C1), the duration in 64ths, a triplet bit and the articulation; there are also
tempo words and bar lines.

READ TAPE reads without halting.
It starts the reader, polls its status while the 340 keeps drawing, and takes each word as it comes.
The tape is over when no word comes for about 2 seconds, 12 for the first read.
A failed checksum takes the block as read, as *pdp1m13* does on Continue.

**The orchestra block.** PUNCH punches the parts exactly as read, checksums included, then one more block.
Its count, 0700050, is negative, so *pdp1m13* halts on it rather than taking it for a part.
Its 40 words are the version, 3, then *per*, the parts a movement, then for each converter nine words, its part
plus 1 (0 for none), its octave, its volume in 1/8 dB, its detune (always 0), its place, 0 to 0400 in steps of
040, and four stops, each a voice number plus 1 (0 for none), then the room and the onset.
The room is *dye*'s code and the onset *dyo*'s, 1/8 ms, with 0400000 for the configuration's.
The checksum follows.

READ TAPE also takes the version 2 block, 0700046 and 38 words, the same without the last two, and sets the room and
the onset to the configuration's for it.
A version 3 block whose room or onset isn't in the menu's lists is refused whole, as a voice the drum doesn't have
is.
The player gives the room and the onset to the device as it does the volumes, when the orchestra is set and before
each piece plays.

The block holds numbers, not addresses or slider positions, a tape remains valid if a library is rebuilt.

### Timing

The timing is *pdp1m13*'s.
A unit, 1/1536 of a whole note, at tempo T lasts K/16 cycles, K = (((T x tpg / 170) x 4808 >> 8) x 35) >> 4,
from Samson's 175 usec loop of 35 cycles; *tpg* is the test word's tempo scale, 170 when as written.
A whole note at 170 is 670,272 cycles, 3.35 seconds.
Every bar starts on all voices together and lasts as long as its longest voice.
A tempo word changes every voice's tempo from the score time where its voice reaches it.
Times are kept from the start of the piece in 1/16 of a cycle, nothing drifts.

### From notes to the converters

The player decodes a bar at a time.
Pass 1 sums each voice's bar and puts its tempo words in order.
Pass 2 runs each converter's part through its sequencer which writes entries to the converter's ring.
An entry is four words: the event's length, the table, the step and the *dyx* value, 0 meaning latch nothing.

The sequencer's rules, in short:
- A note is a strike, key on and the phase to 0, or after a legato note a slur the step changes and the envelope
  carries on.
  A slur on a voice with no sustain level, which would die away under a legato line, is struck again.
- Each note is followed by a key off for its articulation's silence, unless that gap is under 400 cycles, then
  the note sounds on into the next.
- Rests, a voice short of its bar and a movement's end are key offs, ties and offs while silent are ignored.
- An event over 131,071 cycles is split so every event fits the 18-bit event length.

A bar is decoded only when every ring has room for 128 entries, more than any bar needs.

**Feeding the converters.** The service routine, *svc*, polls each converter's flag.
On a new event it latches the next entry from the ring and commits it to the queue with *dyq*.
At the start the player commits entries 0 and 1, starts all four converters with *dxg*, and latches entry 2 so it
always runs two entries ahead.
A new event then has to be served within the two events after it, at least 1,600 cycles, not within one.
The decoder calls *svc* from every loop that could run long.
On the Little Fugue, with the play screen and its readouts, the closest the service ever comes to late is
19,567 cycles to spare.

At PLAY the player clears the four flags, gives each converter its volume and place about 0.1 s before the
start so their slews are over before any sound and latches each converter's fixed values from its voice's
record.

### Loading the voices

At PLAY the loader reads, for each converter in turn, its voice's header into the converter's slot in bank 1,
then the voice's blocks into banks 5-15, one after another from 050000.
A voice two converters share is loaded once.
Every block's ones-complement sum is checked against the header.
When the drum's serial and the voices are the last loaded nothing but the directory is read.

Each read is started before the arithmetic on the block before it.
A read is issued in parts, to the nearest of the read's end, the core bank's
end and the drum field's end, and the next is under way while the last is summed.
The Little Fugue's Flentrop registration, four converters of three or four stops, loads in about 3.3 seconds, the
nine base instruments in 1.7.

**The premix.** A converter's organ stops are added together as they load.
For each zone the loader builds a 1,280-word block.
Word i is the sum of every stop's table word i mod 256 and, below 1,024, the first stop's speech word i,
the division's chiff.
The block plays as a sampled note whose loop is its last 256 words so a converter sounding four stops costs no
more than one.
The stops must agree in their period, envelope, steps and frame, which is what makes them a division.
The voicing combines too, the loudest edge, the largest celeste, the speech and the room if any stop has them.
Each stop's table is a quarter of full scale at most so four of them add to no more than 1,944 of the converters'
2,047, and nothing is shifted or clipped.

### The octave a note sounds at

A voice is in zones of an octave, each holding only the harmonics below 0.45 of the sample rate at its top note
so no note aliases.
A seventh zone, pitches 64-75, cut for C#7, is past every pitch a tape can hold and only a note played an octave
up reaches it.
For a note written at p with o octaves up, p rises an octave at a time while it can, then an octave breaks back
while the note would pass the voice's top, or its step would pass 17 bits, as an organ's high ranks break back.
A voice without the seventh zone, the sampled and recorded voices and the Terts, has its top zone cut for B6
instead.

### The menu

The menu is a Type 340 display list in bank 0 that the 340 runs on its own about 59 frames a second.
The program patches its words only while the 340 is stopped at a frame's end so a frame never shows half a
change.

Everything the pen may hit is drawn with the pen enabled, nothing else is, so a hit is always on a live target.
The pen's aperture is set every frame, wide on the menu and narrow in a list, whose rows are closer.
This isn't authentic, but it's a minor violation to avoid mis-hits.

ROOM and ONSET open lists as the other fields do; a row chosen goes to the device at once by *dye* or *dyo*.

**The lists** are built in bank 1 when one opens and the screen's first word, the gate, jumps to the list in
place of the menu's body.
The gate is written last when a list opens and first when it closes.

**The sliders** take some care.
A pen covers two or three of a track's points and need not hit every frame so a hit alone would make a pen held
still flicker between neighbors.
The hits of one frame on one track are a run, and a run counts only together with the next frame's run on
the same track.
The level is kept if it lies within the two runs' span, else it moves to the span's nearer end.
A pen held still keeps its level and a drag pulls it along a frame behind.
The new level goes to the converter at once, by *dzg*, whose slew keeps a drag from clicking.

While a piece plays the player's idle turn polls the pen so STOP and the sliders work
While it decodes a bar it doesn't poll so in a dense passage the screen can pause for a few milliseconds.

## The voices and the drum

*voices/README.md* has the formats in full.
In short:

**A voice file** is plain text: the sample period N, the mode, the envelope's latch values, the timbres' stride
and thresholds, a sample's end and loop, the step of every pitch, the block every pitch plays, and the blocks'
words in octal.
It holds no address, the player places it.
A library, *.lib*, lists voice files, and a voice's number is its place in the list.

**The drum.** **dacdrum** writes the whole drum from a library.
Field 0 is the directory: a magic word, the version, the library's serial, the voice and family counts, a sum,
the 16 family names as 340 text, then each voice's name as 340 text, where its header is, and its family, with
the sign bit set for a stop.

Each voice's header and blocks follow in fields 1-31, a header whole in one field, and a block written once even
when several voices share it, as a division's chiff is.
A block longer than a field runs on into the next fields.
**dacdrum** writes beside the drum file and renames onto it, so a running emulator takes the new drum whole at
its next transfer.

**How the voices were made:**
- **The basic nine**: five tables synthesized from their harmonics and four sampled notes, organ pipes with a chiff
  and a plucked string, synthesized as sampled notes, one an octave.
- **The Flentrop's stops** are made by **dacvoice** from each stop's pipe class, open, stopped, conical or reed,
  its ranks placed as harmonics of the division's frame, and a level.
  A mutation is a rank on the 3rd, 5th or 6th harmonic, exactly in tune; a mixture is several ranks, each breaking
  back an octave where it would pass a zone's top.
- **The bagpipe's** chanter and drones are tables made by **dacvoice** from their harmonics, the chanter's two
  formants and the drones' odd harmonics, the chanter at a period of 7 cycles so its images fall above hearing.
- **The recorded instruments** were cut from the recordings by tools that are not part of the release.
  A zone is four semitones, its root a recorded note.
  Each zone is band-limited to what its highest note can play below 0.45 of the sample rate, resampled with a
  windowed sinc to 1.25 times that band's Nyquist rate, and for a solo instrument flattened to a steady pitch, so
  every period is a whole number of words and the loop is seamless; the vibrato is then the device's.
  The loop is two or more periods crossfaded into the attack, at least 64 samples.
  A piano or guitar note's decay is measured and taken out of the stored words, and the device's envelope puts it
  back.
  Each voice is packed.
- **The Hammond** is a stop with a 16-foot frame.
  The tonewheels are sines and the drawbars its ranks, the key
  click its chiff, the Leslie the slow counter running free.
  It is not interpolated, four-note chords interpolated would take 80% of the channel.

## The renderer

*dacrender* is the IOT and the player rebuilt on the host.
It decodes the tape as the player does, makes the
same entries, places and premixes the voices the same way, and runs the four converters a machine cycle at a time
through the same integer arithmetic.
The analog levels and the mix are floating point on both, so a render and the device's output agree to within
rounding, not bit for bit.
*room.h* and *edge.h* are the device's own.
The gain table is written as numbers and the Makefile turns off fused
multiply-adds, so every host produces the same file.

It renders to the end of the two-second pause after the last movement, plus the room's tail.
It takes the room and the onset from a version 3 orchestra block unless *--room=* or *--onset=* is given; with
neither, the room is none and the onset off.
Left out, since they are not on the tape: the test word's tempo scale, sense switch 1's repeat, and a slider moved
while the piece plays.

## What it costs

| What | Share of the machine's cycles |
|---|---|
| The four converters on the Little Fugue, the Flentrop with the celeste | 48.5% over the piece, 60.5% at its densest |
| The same without the céleste | 32.3% |
| One recorded voice, packed | 4.2 to 8.8% |
| The Hammond | About 18% over a minute of playing, 40% in four-note chords |
| The Type 340 drawing the play screen | about 10.5% |

The room, the onset and the voicing's noise cost the PDP-1 nothing; they are host work.
