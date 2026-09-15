# Lunar Lander for the Type 340

`lander340.am1` re-implements Michael Gardi's Lunar Lander for am1 and the Type 340 display.
The rules, physics, scoring and controls are the original's.
The drawing is new: a resident 340 display list, patched between refreshes.

## Playing

```
make                      # or: am1 lander340.am1
```

Load `lander340.rim`, then:

- `start 5` reads the controls from the test word switches;
- `start 4` reads them from the Spacewar control boxes;
- `start 6` plays with the lightpen (see below).

Both control boxes work as in the original.

"NEW GAME" is look-at-me mode, the LEM drifts across the screen and any press-and-release starts a round.
Land upright, falling slower than 100 and drifting slower than 50, on one of the six pads.

- **Landing:** scores the pad's multiplier × 50 and adds 100 units of fuel, lighting that
  pad's program flag.
- **All six pads:** pays a 1000-point bonus.
- **Crash:** costs 100 units of fuel. It scores 5 × the multiplier inside a zone, 5 outside.
- **Game over:** when a crash leaves no fuel. Press and release to go back to "NEW GAME".
- **Sense Switch 1:** changing it during play restarts the game, including during an explosion

### With the lightpen (`start 6`)

Three boxes at the top of the screen stand in for the switches: `< LEFT`, `THRUST` and
`RIGHT >`.
- **THRUST:** thrusts for as long as the pen is held on the box (left mouse button down).
- **LEFT and RIGHT:** each touch rotates one step. Lift the pen and touch again for the next.
- **Held boxes** are drawn brighter.
- **NEW GAME and GAME OVER** are targets too. Touch the words and lift the pen. The pen has to
  be on a letter; the gap between the two words is wider than the aperture.

## How it draws

The original drew everything from the CPU, one `dpy` per point, every frame; drawing *was*
the frame time.
Here the 340 runs one display list on its own, and the CPU's jobs each
refresh are one `dla`, a delay, and a few patched words.

The list `dlist`, in drawing order:

1. **Terrain:** one chain of 99 vectors, built once by `bterr` from the original's height
   table `ypt`. It relies on each vector ending exactly at its endpoint.
2. **Landing pads:** brighter, with a short post at each end.
3. **Pad multipliers:** in the character generator, at scale 1.
4. **Status lines:** FUEL, SCORE, and VERT and HORIZ with direction arrows. The numbers are
   packed character words that `fmt6` rewrites with `div`, and only when the value changed.
5. **Lightpen targets:** the three boxes, in `start 6` only.
6. **Messages:** NEW GAME, GAME OVER or BONUS, at scale 2.
7. **Explosion debris:** 56 point-mode words.
8. **The LEM and its flame:** vector blocks called with `save`, five orientations, and four
   flame lengths per orientation picked at random while thrusting.

Anything that appears and disappears sits behind a switch, a subroutine-mode `jump` word the
CPU points either at the block or just past it.
A block that is off costs the 340 nothing.

The 340 does not clip, and an edge violation halts it.
- debris points that are off the screen are moved to, not shown
- the LEM's drawing position is held inside per-orientation limits, computed from its vectors

### The lightpen

`start 6` sets the pen's aperture to 28 raster units and enables the pen for two things only:
- the target boxes;
- the message, in attract mode and at game over.

### The LEM shape

The LEM is drawn as polylines, cabin, descent stage, legs with footpads, engine bell.
It rotates them about (0, −4), the middle of the original's collision box, so every
orientation stays inside the box the game tests.
If you change the shape run `make gen`.
The generated block sits between the `GENERATED-BEGIN` and `GENERATED-END` markers.

## Pacing

The original had no timer; its speed was however long a frame took to draw.

| | original (measured) | this version |
|---|---|---|
| game tick in play | ~78 ms (12.8/s) | 4 refreshes, ~76 ms measured |
| explosion step (32 steps) | ~98 ms | 5 refreshes, ~100 ms |
| landing score count-up | ~28 points/s | 2 refreshes a point, ~25/s |
| landing fuel count-up | ~18.5 units/s (slowed by its digit conversion) | 3 refreshes a unit, ~17/s |
| bonus count-up | not measured | 2 refreshes per 10 points, ~4 s for 1000 |

One refresh is `REFRESH` passes of the delay loop:
- the loop is 750 passes of `dsp`, `jmp`, `isp`, `jmp`, 25 µs each;
- that makes a refresh about 20 ms;
- the emulator measured ~51 refreshes a second.

The 340 draws the whole list in about 11 ms of that, measured.

## What is kept exactly

- **Physics:**
  - gravity is 2 a tick;
  - positions move by velocity / 32;
  - the field is 0–8000 game units, one screen unit per 8;
  - leaving the field during play starts the round again.
- **Thrust:** the components per orientation (−6, −3, 0, 3, 6 across; 0, −3, −6, −3, 0 up)
  come from the orientation of the tick before, as in the original.
- **Collision:**
  - the box is x −7…+23 and y −18…+9 around the LEM's screen position, tested against the
    100 terrain points;
  - the zones are the original's `lzx`/`rzx` tables;
  - a soft landing needs vy < 100, |vx| < 50, upright.
- **Scoring and fuel:** the `skz` and `sk2` tables, 5 outside a zone, ±100 fuel, the bonus
  when `cbn` reaches 077, the program flags.
- **Random numbers:**
  - the same generator, called in the same order: once per thrusting tick for the flame, and
    four times per particle (x, y, dy, dx);
  - so the same inputs scatter the debris the same way.
- **Controls:**
  - rotation steps once per press (edge-triggered);
  - pressing both directions is ignored;
  - thrust needs fuel.

## Deliberate differences

1. **Attract mode no longer starts a game on its own.** In the original the right-edge check
   in look-at-me mode never fires. The LEM drifts out of the field, and the motion routine's out-of-bounds
   exit starts a round.
   Here the LEM wraps back to the left edge at x 7960 (`GWRAP`) and look-at-me mode waits for a press.
2. **Pad multipliers are drawn every refresh.** The original drew them one frame in 50, which
   flickered. As in the original, they are hidden from a crash until the next round, and only
   the pad just landed on is shown during a landing.
3. **`start 6` plays with the lightpen** instead of the IOT 15 joystick.
4. **Pacing comes from a delay loop**, not from drawing time (see above).
5. **The whole status display stays up during the landing count-ups.** The original showed
   only the number being counted, plus BONUS. The explosion still shows only the terrain and
   the debris, as the original did.
6. **Near a screen edge the LEM is drawn slightly inside the screen** rather than off it. The
   game's position and every collision test are unchanged. At the start of a round, and
   anywhere away from the edges, the LEM is drawn exactly where the game has it.
7. **The explosion is 56 points redrawn every refresh**, fading from intensity 7 to 3 over the
   32 steps. The original plotted each step once, one main-loop frame in 16, and relied on
   the phosphor.
8. **The LEM and flame are vector drawings, not the original's bitmaps.** The outline has the
   same size and orientations.

## What changed for speed

- **Drawing:** the CPU no longer draws; the 340 does. The CPU spends most of each refresh in
  the delay loop.
- **Collision:** `lti` examines only the terrain points under the box. It starts from an index
  computed from the box's left edge and stops past its right edge. That gives the same answer
  as the original's scan of all 100 points.
- **Digits:** `fmt6` converts with `div` by 10, not repeated subtraction. The status fields
  are rewritten only when their values change.
- **Terrain:** built into vectors once at start-up, one chain with no point words after the
  first. A point word costs the 340 far more than a vector.
