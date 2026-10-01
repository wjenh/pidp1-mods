# Rain Round -- design

This is what was produced by a challenge given Claude Opus 5.5:

Create a program in PDP-1 assembler that plays 4 part polyphonic music at the same time displaying a
dynamic graphical image.
No audio dropouts, tone shifts, display tearing is allowed.
You will be given no more direction, all decisions are to be made by you.

It was originally written 22-Sep-2026 by Claude in minutes.

## 1. Audit: what the environment gives

| Question | Finding | Source |
|---|---|---|
| How does sound get out? | Program flags PF1-PF4 are sampled by the emulator's audio thread into four low-pass-filtered channels (the CHM PDP-1's RC filters, per Samson's notes). PF5/PF6 are not heard. | `Docs/UsingAudio.md`, `pidp1.config` (alpha1-4 "pf1..pf4") |
| Is the audio time base the CPU's? | Yes: samples are taken on simulated time (5 us granularity), so a program's cycle count *is* its pitch. | `pidp1.config` samplerate note |
| Flag instructions | `stf n` / `clf n` are operate-class, 5 us, no memory cycle. `szf` is a skip. | am1 skill, `devices.md` |
| Instruction times | Memory reference 10 us, +5 us per indirection; `jmp jsp law` operate, shift, skip, and an IOT **without** the wait bit: 5 us. `mul`/`div` are data-dependent (14-40 us) -- banned from the loop. | F-15D handbook via am1 skill |
| Display choice | **Type 30** (`dpy`, IOT 7).  Issued without the wait bit (`dpy-i`), one point costs a fixed 5 us of CPU and the display completes on its own. The Type 340 was rejected: on real hardware, it fetches its display list over a high-speed channel, stealing memory cycles at times the program cannot predict -- exactly the drift constraint forbidden.  | `UsingT30dpy.md`, type340 skill "Timing" and "Instruction caching" |
| Type 30 point rate | A point needs ~50 us before the next; this design never issues two closer than 60 us. | DEC Type 30 spec |
| Coordinates | x in AC, y in IO, the high 10 bits of each, signed, 0 = center. Intensity in bits 6-8 of the instruction (`dpy-i 300`). | `FunStuff/Demos/drawLines.am1` |
| Observing it headless | ad1 `monitor N file` single-steps N instructions and logs each executed address+word: an exact execution trace from the real emulator. The display's point stream is TCP 3400 (`DisplayClient`). | `Docs/UsingAD1.md`, `pidp1-test-harness` skill |
| An independent clock | The BBN clock (`rck`, IOT 32) returns a 1 ms counter. A measurement build reads it once per frame. | `Docs/UsingTimesharingClock.md` |

## 2. Architecture: one tick, one job

Everything happens in a **tick** of exactly **60 memory cycles = 300 us**
(3333.3 ticks per second):

```
cycle  0- 1   dap job        AC holds the address of this tick's job
       2-37   4 voices       9 cycles each, fixed-cost, branch-free
      38-39   jmp i job
      40-58   the job body   exactly 19 cycles on every path
      59      jsp tick       the job's last word; its return address IS the next job
```

**Audio (36 cycles).** Each voice is a phase accumulator (DDS), the flag
following the sign of the phase:

```
    lac ph1; add in1; dac ph1       // 30 us: phase += increment
    sma; clf 1                      // negative half: flag stays clear...
    spa; stf 1                      // ...positive half: set it
```

Exactly one of `clf`/`stf` runs and the other is skipped, so both halves
cost 15 us.
Pitch is `f = inc / ((2^18-1) * 300 us)` -- a resolution of
0.0127 Hz, no drift ever, because the phase carries its remainder forward
instead of rounding a period to whole ticks.
(Ones'-complement end-around carry makes the cycle 2^18-1, not 2^18, and the song compiler uses that.)
The price is edge jitter of up to one tick (300 us), the same kind of
quantization Samson's loop had; the voices are kept below ~660 Hz so each
period spans at least five ticks.

**Everything else is a job.** A job is a straight run of code whose body
costs exactly 19 cycles on every path, ending in `jsp tick`.
`jsp` leaves the address of the *following* word in AC, and the tick stores it with
`dap job` -- so the job list is simply laid out in memory in execution
order, and "which job is next" costs nothing.
The last job of the frame ends `law first; jmp tick` instead (the same 2 cycles as the `jsp` plus
one spare, so its body is 18).

A job that must decide something uses a balanced branch, both arms padded
to the same count:

```
    spa             // skip if not hit
    jmp hit
    ... not-hit work, padded ...
    jmp join
hit, ... hit work ...
join, jsp tick
```

An arm may also live out of line, reached by a `jmp` and returning by
one: DROPB's hit arm needs the fall-through, so its miss arm's padding
sits after the last job. The checker follows jumps wherever they go, so
this costs no proof.

So the **frame** is a fixed list of jobs, and its time is fixed too:

| Jobs | Count | Work |
|---|---|---|
| LOOP | 1 | at the loop point, repoint the four voice sequencers |
| SEQ k | 4 | if this frame ends voice k's note, load the next note's increment, flag a droplet |
| DROPA k | 4 | spawn a droplet at the spout, or apply gravity to a falling one |
| DROPB k | 4 | collision with the water; on a hit, a fixed velocity kick (32 units down) on that node, and the droplet returns to its spout |
| WAVEV i | 30 | velocity: `v += ((y[i-1]-y[i]) + (y[i+1]-y[i]) - (y[i]-level))/4`, tension plus the pull back to level |
| DAMP | 15 | `v -= v/32`, two nodes per job |
| WAVEY | 10 | position: `y += v`, three nodes per job (all velocities are done first) |
| PLOT | 34 | 32 surface nodes + 31 interpolated midpoints + 4 droplets = 67 points, two per job (the last has one), 70 us apart within a job and at least 60 us across jobs. The last droplet's job has 14 spare cycles; the `-DMEASURE` build spends 12 of them logging the BBN clock |
| FRAME | 1 | frame counter, song loop, back to the first job |
| **Total** | **103** | 103 x 300 us = **30.9 ms per frame, 32.4 frames/s** |

The whole program is unrolled: no job contains a loop, so there is no
data-dependent iteration anywhere after start-up. The only variation is
the balanced branches, and those are what the validation tools check.

**Physics.** 32 nodes of a 1D string (the water surface), ends fixed at
the level (y = -100). Symplectic Euler (velocity first, then position)
with tension 1/4 and a restoring pull of 1/4 toward the level: the
highest mode has w^2 = 1.25 < 4, so the scheme is stable. Damping takes
1/32 of each velocity per frame. Heights are stored as display
coordinates (screen units << 8), so a node is plotted with no conversion
and the low 8 bits are sub-pixel precision. The sum is formed as
`(y[i-1]-y[i]) + (y[i+1]-y[i]) - y[i] + level` so every partial sum stays
near a single height, far from overflow. Each voice owns a spout 300
units up, above columns 5, 12, 19 and 26; every note it sounds releases a
droplet that falls under a gravity of 20 units per frame per frame. The
kick is fixed at 32 units per frame, so the system is linear: steady
state is about 30 units of ripple, with peaks near 40 (measured, section 5).

**Music.** A four-voice round: voice k enters two bars after voice k-1,
voices in different octaves (G major, 73-659 Hz). Notes are lists of
`(increment, end frame)` pairs generated offline by `tools/gensong.py`
into `song.ah`; an 8th note is 7 frames (216 ms, 139 bpm) with its last
frame silent for articulation. After the six-bar entry the eight-bar
steady state loops forever.

## 3. Cycle-counting strategy

Three independent checks, each able to fail:

1. **Static proof** (`tools/cyclecheck.py`). Reads the assembler listing,
   decodes every word from the tick to the last job, and enumerates
   *every* path through every job (both arms of every skip), summing
   handbook times. Fails unless the tick prologue is 40 cycles, every
   path of every job is 20 (19 + exit), no loop-resident instruction has
   a variable time (`mul`, `div`, a waiting IOT, a `hlt`), and every pair
   of `dpy`s, across job boundaries too, is at least 12 cycles apart.
2. **Execution trace** (`tools/tracecheck.py`). Runs the real program in
   the emulator, stops it at chosen moments (quiet water; droplets hitting;
   the song's loop point), and has ad1 `monitor` several thousand
   instructions. Replays the trace against the same timing table: every
   tick must be 60 cycles, every flag edge must land on its fixed cycle
   offset, each voice's measured period must match its note, and branch
   coverage records that both arms of the conditional jobs actually ran.
3. **The emulator's own clock** (`-DMEASURE` build). Spare cycles in the
   last plotting job are spent on `rck` into a ring buffer; after N frames
   the measured milliseconds must equal N x 30.9 ms to within the clock's
   1 ms resolution. This does not trust my timing table at all.
4. **The sound itself** (`tools/audiocap.py`). The emulator's audio is
   captured through SDL's disk driver, and the notes' pitches and onsets
   are measured from the samples.

Every check has a control leg: a `-DCONTROL_SLOW` build adds one `nop`
to the tick (61 cycles), and each check must fail on it.

## 4. What first light changed

The first build was 88 jobs (26.4 ms per frame), and it passed every timing check.
The rendered display showed that the physics was wrong,
though, and each fix cost jobs, which the frame absorbed without
touching the tick:

- **The surface sagged into a trough.** Each kick pushed the surface
  down, and a pure string has nothing that pulls it back. A restoring
  term toward the level went into WAVEV, and damping moved out to its
  own 15 DAMP jobs to make room. The frame went to 103 jobs, and the 8th
  note to 7 frames to keep the tempo.
- **Kicks taken from the penetration made the surface run away.** A
  rising surface meets a falling droplet deeper, so a
  penetration-scaled kick fed the waves back into themselves. At twice
  the penetration, the surface filled the screen within 9 s. The kick is
  now a fixed 32 units, chosen with a float model of the same scheme
  (peak 83, typical 42 under random note timing). In the emulator it
  held a 39-unit peak over three song loops.
