# Rain Round 340 -- design

After the first demo using the Type 30, Claude was challenged again,
"Make it work on the Type 340".
As you can see, not only did Claude make it work, the results are, well,
amazing, and again, the above direction was all that Claude was told.

Now in Claud's own words:

Rain again with the animation moved from the Type 30 to the Type 340.
The same four-voice round plays on the program flags in
a single loop of fixed ticks, 73 program cycles, 85 with the 340's steals.
The 340 draws a much richer picture from display lists:
continuous water lines, layered depth lines, droplet sprites, expanding
splash rings, rain clouds and a title.

Are you totally impressed yet?
Note that this would run on actual PDP-1 with a Type 340 display
and Peter Samson's audio output hack.

On to Claude's design.

## 1. The problem the 340 brings

On a real PDP-1 the 340 fetches every display instruction from core over a
high-speed channel, and each fetch takes a memory cycle from the CPU.
A program whose audio pitch is its cycle count hears every stolen cycle:

- A steady number of steals per tick only lowers the pitch by a constant
  factor, which the song can be tuned for.
- Steals that come in bursts change the tick length inside a frame. That
  would be heard as a buzz at the frame rate. It is why Rain Round chose
  the Type 30.
- A steal that lands between the start of a tick and a voice's flag
  operation moves that edge. If the steals land at varying points, the
  edges jitter.

And one problem any tick-stepped voice has, which the tick's length
decides, a voice's flag is the sign of a phase accumulator stepped once
a tick, so its edges fall on the tick grid.
Unless a note's period is a whole number of ticks, the edge pattern repeats at a rate of its own.
When that rate is 15-80 Hz, it is heard as flutter, a very fast staccato.
At 4-15 Hz it is heard as tremolo.
No filter removes either, since both sit within a few tens of Hz of the note.

## 2. What the emulator does, measured

- **Each 340 fetch steals one cyclet**
  This program's 12 fetches a tick make its 73-cycle tick 85, and the emulator keeps real time.
- **The steals land when the emulator's 340 thread makes the fetches,
  in host time,** so within the first part of the tick after the `dla`, this happens at varying points.
  Voices placed right after the `dla` could have their edges scattered by 20-24 us rms.
- **It enforces the 340's timing:** 5 us per fetch,
  35 us per point, 1.5 us per vector step.
- **A tick in which the 340 sat halted makes the next list far more
  likely to be cut** (2-5% of the lists that follow an idle tick,
  against about 0.1% otherwise), so no tick is left without something to draw (section 3).

## 3. Architecture: a display list per tick

Every tick starts one short **sub-list**, at a fixed cycle of the tick,
in program cycles; the 340's 12 steals come on top, early in the tick:

```
cycle  0- 1   dap job        AC = this tick's job (jsp's return address)
       2-14   TICKPAD        padding nops: the tick's length (below)
      15-50   4 voices       as in Rain Round, 9 cycles each
      51      dla            IO = the sub-list the previous job chose
      52-53   jmp i job
      54-69   the job        exactly 16 cycles on every path
      70-71   lio [list]     the sub-list for the next tick
      72      jsp tick
```

**The voices come last before the `dla`,** 36 program cycles after the
previous one.
The 340 fetches its list in the first part of the tick,
so its steals land before the voices, not among them (section 2).

**The tick's length is chosen for the song.** 73 program cycles and 12
steals make 85 (425 us).
On that tick, the song's commonest notes have
periods of nearly whole ticks, G in three octaves (12.00, 24.01 and 6.00
ticks), D3 (16.02) and C3/C4 (17.99, 8.99).
Therefor their edges repeat at 1-4 Hz, which is heard as a steady tone.
A pattern at 4-15 Hz is heard as tremolo, and at 15-80 Hz as flutter.
G on 85 has the least of both.
TICKPAD is generated, and the tempo and gravity follow.

The rules that make this safe on real hardware and in the emulator:

1. **Every sub-list fetches exactly K = 12 words and ends in `halt`.**
   A list with less to draw is padded. One or two pads are no-op
   parameter words. Three or more are a 340 subroutine call into a
   shared ramp of zero-length vectors, entered at the depth that makes
   up the count (`art.pad_to_k`). So every tick loses exactly 12
   cycles, on a real PDP-1 and on the emulator alike, and the tick is
   still exactly constant. The song is tuned for the 85-cycle tick.
2. **Every sub-list finishes within its tick.** In DEC's model, K
   fetches, one point and the vector steps must fit 140 us, which allows
   30 steps and 7 vectors (`art.MAX_STEPS`, `art.MAX_VECTORS`). The
   generator asserts this for every list type, including the
   data-dependent water segments at their worst slope.
3. **Every sub-list starts from an absolute position** (a `y` point word,
   then an `x` one), so a list that went wrong could not displace the
   next.
4. **No tearing.** A display word is drawn only in ticks outside the run
   of jobs that writes its group. The water is drawn from the previous
   frame's finished words while this frame's physics runs.
5. **No idle ticks.** The frame's 120 ticks are filled exactly:
   - the 57 water lists and 4 rings are drawn once;
   - the 4 droplets are drawn twice;
   - the 12 cloud chunks and 5 title chunks are drawn three times each.

   Repeats are spread evenly, and every chunk of a thing is drawn the
   same number of times, so no part outshines the rest.

## 4. The frame

| Jobs | n | Work (16 cycles each) |
|---|---|---|
| LOOP | 1 | at the loop point, repoint the voices |
| SEQ k | 4 | at a note's end frame, point voice k at its next note |
| TUNE P/A/B/C/D | 5 | retune one note-table entry from the switches (section 5) |
| SPAWN k | 4 | voice k moved to a note: ask for a droplet |
| CLOUD k | 4 | cloud k bright while voice k sounds, dim while it rests |
| DROPA k | 4 | launch a droplet from its cloud, or apply gravity |
| DROPB k | 4 | hit test; on a hit, kick the node and start the splash ring |
| DROPC k | 4 | a droplet that has just hit goes back to its cloud |
| DROPD k | 4 | the same hit kicks the node's neighbors by half |
| RINGA k | 4 | step the ring: parameter word and sprite from a table |
| RINGB k | 4 | hold a finished ring at the table's invisible tail |
| DROPY | 2 | droplet y anchors, two per job |
| WAVEV i | 18 | `v += (y[i-1] + y[i+1] - 3y[i])/4`, heights relative to the level |
| DY i | 18 | `v -= v/32`, `y += v` |
| RINGY | 2 | ring y anchors, from the water at their columns |
| VEC i | 19 | segment i's vector word, from `y[i+1]-y[i]` |
| WANCH j | 18 | anchor y words for water list j, all three layers |
| FRAME | 1 | frame counter and song loop |
| **Total** | **120** | 120 x 425 us = **51.0 ms per frame, 19.6 frames/s** |

An eighth note is 5 frames (255 ms), and a droplet's gravity (36 raster
units per frame squared) brings it down in one eighth. Both were 7 frames
and 24 on the 36 ms frames of the first program, the same in real time.
The water's constants are per frame and were not changed, so the waves
move about 30% slower in real time than they did then.

**Physics** is Rain Round's, with 20 nodes. Heights are stored relative
to the level, as raster units x 128, so the restoring term is plain `-y`
and each anchor word is `y >> 7` plus a constant. At scale 1 a vector
step is 2 raster units, so `|dy|` in these units is already the delta
field in bits 8-14. A hit kicks the node by a fixed amount and its two
neighbors by half. A [1/2, 1, 1/2] kick gives nothing to the
highest wave mode, where neighboring nodes swing opposite ways. That
mode draws the steepest segments, which are the slowest for the 340.

**The animation:**

- **Water:** 19 vectors node to node at scale 1, one per sub-list. Each
  sub-list calls its one-word vector block as a 340 subroutine (`save`).
- **Depth lines:** the same blocks drawn twice more, 70 and 140 raster
  units lower and dimmer. They cost the CPU only two more anchor stores
  per list.
- **Droplets:** a 28-step teardrop sprite, anchored at the droplet's
  height.
- **Splash rings:** a hexagon that grows through scales 0-3 and fades as
  it goes. When finished, it switches to an invisible copy with the same
  fetch count.
- **Four rain clouds** at scale 2 (three chunks each), lit by their
  voices, and the title "FRERE JACQUES" in five chunks.

## 5. Operator tuning

Sense switches 1-5 are an amount from 0 to 31, switch 1 the highest
bit. Switch 6 lowers the pitch instead of raising it. Every note sounds
at base x (1 +- a/128), a step of 0.78%, up to 24%.

- **Note table:** the song's 20 distinct increments, a rest's 0 first,
  are a base table `nb` of 32 entries, with a tuned copy `nt` 040 words
  on. The song's pairs name `nt` entries.
- **The voices:** SEQ points the voice's `add` instruction at the note's
  entry, so the voice reads the tuned value on every tick. A held note
  follows the switches too, and the tick stays 9 cycles a voice.
- **Retuning:** the five TUNE jobs retune one entry per frame, so a
  switch change is heard within 32 frames (1.6 s). `begin` tunes the
  whole table at start, so a song started with the switches set is in
  tune from its first note.
- **The arithmetic:** base/8 x a/16, by Horner's rule over the five
  switches, most significant switch last. Each switch test is
  `szs n; add bq; szs i n; add [0]`, 4 cycles on or off. The negation
  is `szs i 60; nop; szs 60; cma`, 3 cycles either way.
- **Rests stay +0:** a rest's 0 comes through as -0 + +0, which the
  PDP-1 gives as +0.
- **SPAWN and CLOUD** see a note change as a change in the voice's add
  word, and read the increment through it.

Settings: all off plays the song as written, in tune on a real PDP-1
and on any emulator host that keeps real time.
A host that falls behind real time plays flat by its shortfall, which the switches take back by ear.
