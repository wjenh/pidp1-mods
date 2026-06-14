# PDP-1 Emulator — Session Notes

Carried over from a Claude Project chat ("PDP-1 emulator refactoring"), prior
to migrating to a Linux-based Claude client.

## Coding standard / approach established

- The project has a documented coding standard (`LLMCodingFormat.md`), section
  4 of which mandates defensive parentheses around mixed-precedence
  arithmetic/logical/bitwise expressions.
- When modifying a source file, **add a revision line in the comment block at
  the top**, matching the existing revision-history style.

## Naming philosophy decision (important — don't redo this)

Discussed and settled: **do not rename the hardware-tied variables in
`pdp1.c`** (`ac`, `io`, `mb`, `ma`, `df1`, `df2`, `bc`, `ihs`, `ios`, `scr`,
`smb`, `srm`, `b1`-`b4`, etc.). Rationale:

- These names are direct handles into the PDP-1 Maintenance Manual and
  schematics — they're the index for cross-referencing timing/flip-flop
  behavior against real hardware docs.
- The user's emulation philosophy is "black box" (behavior-correct from the
  outside, like simh) but `pdp1.c` itself is the *inside* of that box —
  reproducing internal flip-flop-level state — so hardware names stay.
- Approach instead: documentation comments explaining what each macro/TP block
  *does* in plain terms, while names still say what it *is* in hardware terms.
  This gives readability without losing traceability or risking subtle bugs
  from a rename pass.
- If/when there's a genuinely external-facing surface (IOT handlers, panel
  interface, `handlecmd`), behavioral naming makes sense there — not in the
  core cycle/SBS/ALU state.

## Architecture: clean separation already done by user

- `display.c` and the dynamic IOT architecture were moved out of `pdp1.c`.
  Original code had display logic embedded in the emulator core, which wasn't
  true to the real machine (display was a separate bridge).
- New IOTs are added by dropping a compiled `.so` into the proper directory —
  no emulator modification needed. This mirrors how the real PDP-1 treated I/O
  devices as pluggable peripherals on the IOT bus.
- This separation made implementing both Type 30 and Type 340 display hardware
  much cleaner.

## Work completed

1. **`pdp1.c`**: Full documentation + defensive-parenthesization pass
   completed (cycle-state macros, `mul_shift`/`div_shift`/`pc_to_ac`/`carry`,
   `shro()` rotate table, `sbs_calc_req`, `multiply()`/`divide()`,
   `cycle0()`/`cycle1()` dispatch/skip conditionals, IOT dispatch, reader/
   punch/typewriter bit-packing, `getwrd()`). No logic changes, only added
   `()` and minor spacing (`AC&~B0` → `AC & ~B0`). Passed testing (after user
   restored 4 lines of code that were accidentally dropped during editing —
   watch for this class of error).

2. **`panel_pidp1.c`** (in `src/blincolnlights/pdp1/`, 413 lines): This is the
   high-CPU, real-time-threaded GPIO panel driver (NOT `panel1.c`, which is
   just a 131-line light/switch register mapping file — confirmed via
   investigation when the user's description didn't match `panel1.c`).
   Documentation pass + defensive parenthesization completed on `map()` and
   the decay-filter math; revision line added (12-Jun-26). No logic/naming/
   timing changes.

## Pending: panel_pidp1.c major rework (discussion only, not yet implemented)

User's proposed simplification, agreed as sound:

- **Drop the decay/fade filter entirely** in favor of simple PWM. Real PDP-1
  incandescent lamps have thermal time constants in the tens-of-ms range, so a
  fixed-period PWM looks visually identical to the current decay-filtered
  output — the decay filter was software-emulating something the lamp's own
  thermal mass gave for free.
- **Replace the sampling scheme**: currently the emulator sets/clears one
  light bit per 5µs cycle, and the panel thread polls the whole `Panel` struct
  1000 times over 3ms (`NSAMPLES`) to reconstruct duty cycle. Proposed: have
  `pdp1.c` increment a small per-bit counter (e.g. `u8`, 180 bytes for 10x18
  bits) whenever a lamp bit is on during a cycle; panel thread reads+resets
  the counter ~once per ms. ~1000x reduction in panel-thread polling.
  - Open question: counter reset semantics (panel thread vs emulator) — a
    race where the emulator increments between panel's read and reset could
    lose a tick; probably negligible, but consider double-buffering/atomic
    swap if it matters.
  - This adds a small new responsibility to `pdp1.c`: increment a counter
    array wherever lamp bits are currently OR'd into `lights0..9`.
- `lightRow`'s 31-phase/`phase_delays` PWM scheme could likely also be
  simplified once not chasing a smooth decay curve.

### Critical constraint: web app panel display

- There's a second panel display embedded in a web app (`web_pdp1`), which the
  user does **not** want broken. It's believed to have its own ~1ms loop and
  not do the excessive 1000x sampling.
- Plan: keep `lights0..9` bit-setting in `pdp1.c` exactly as-is (web app
  unaffected), and *additively* introduce the new per-bit counter array for
  the native panel driver only.
- **Before touching `pdp1.c`**: confirm what the web app's panel code actually
  reads — if it depends on `p->lamps[][]` (the smoothed brightness array
  written by `lampthread`), removing `lampthread` would break it. User said to
  wait until they resume the session before deciding on work partitioning
  between `pdp1.c` and `panel_pidp1.c`.

## Status at handoff

Cleanup phase (documentation + defensive parens) done for `pdp1.c` and
`panel_pidp1.c`. The panel rework above is scoped and agreed in principle but
**not started** — next step when resumed is to check the web app's panel code,
then split work between `pdp1.c` (counter increments) and `panel_pidp1.c`
(remove decay filter, switch to counter-based PWM).
