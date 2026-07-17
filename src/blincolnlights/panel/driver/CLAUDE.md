# panel/driver/ -- Panel Driver Process Notes for Claude

Scope: `newpanel.c`, `panel_pidp1.h` (the latter lives at the `blincolnlights/` root, shared
with `vpanel_pdp1/` and others) -- the standalone panel driver process that reads
`/tmp/pdp1_panel` shared memory and drives the physical front-panel GPIO. This file loads
automatically whenever Claude reads a file in this directory; it does not replace
`Claude/CLAUDE.md` (project-wide rules, coding standard, sandbox gotchas) -- read that too, it
is not repeated here.

## Source-tree refactor history (16-Jul-2026 and 17-Jul-2026)
See `REFACTOR-CHANGES.txt` at the repo root for the full record. Relevant to this directory:
- Dead code removed: `Pinctrl/pinctrl.c` (vendored standalone CLI tool, `xmain()`, never
  linked into anything), and the top-level `gpio.c`/`gpio.h` and `checkConfig.c` (superseded
  by the Pinctrl library and `pdp1/configuration.c` respectively).
- `panel_pidp1/Old/` (the previous-generation `panel_pidp1.c`) moved to
  `Archive/panel_pidp1_Old/` at the `blincolnlights/` level, out of the live build tree.
  Still the historical comparison point for the panel-flicker investigation mentioned below.
- `Makefile` and `Pinctrl/Makefile` rebuilt to match `pdp1/Makefile`'s dependency-tracking
  pattern (`-MMD`/`-MP`, real per-file objects). Previously a change to a shared header
  (`common.h`, `panel_pidp1.h`, `configuration.h`, `gpiochip.h`, `gpiolib.h`, `util.h`) did
  not force a rebuild of anything that included it. Verified: touching `../common.h` now
  correctly forces `common.o` to recompile. Enabling `-Wall -Wextra` (to match `pdp1/Makefile`)
  surfaced a number of pre-existing sign-compare/uninitialized-variable warnings in
  `newpanel.c` that were previously silent; these are not fixed as part of this pass.
- **17-Jul-2026, phase 4:** this directory itself moved from `panel_pidp1/` to
  `panel/driver/` (one level deeper under `blincolnlights/`), and the `Pinctrl/` subdirectory
  moved out from under it to become a sibling, `panel/gpio/` (see that directory's own
  Makefile comment). The Makefile's `INC`/`PARENT_SRCS`/`PDP1_SRCS` paths gained an extra
  `../` for the added depth, and its `Pinctrl` references became `../gpio`. `newpanel.c`
  itself needed no changes: it includes `"gpiolib.h"` with no path prefix, resolved purely
  through the Makefile's `-I../gpio` flag, and its own mentions of "panel_pidp1" are just
  historical references to the archived `panel_pidp1.c` predecessor, not paths.
  `setpriv.sh`'s two hardcoded install-path references were updated to
  `panel/driver/newpanel`. Verified: builds cleanly against the relocated `../gpio/`, no-op
  rebuild works, touching a shared header still forces the right recompiles.

## Architecture summary
- `panelthread()` (the process's main thread) drives the 10-row light scan (`setLights()` ->
  `lightRow()` per row) and reads one switch register per iteration (`readSwitches()`).
- `pwmthread()` (spawned by panelthread) periodically reads/resets `panel->pwmcount[][]`,
  scales it against actual elapsed emulator cycles (`panel->cyclecount`), applies
  `dimmingFactor`, and writes the result into a double-buffered `lights[2][10][18]` array that
  `lightRow()` uses as its per-column phase-count threshold. See `newpanel.c`'s own top-of-file
  comment (changelog style, one entry per session) for the full evolution of this design --
  it is kept current there, not duplicated here.
- `lightRow()`'s phase sleeps target absolute per-row deadlines (`clock_nanosleep(...,
  TIMER_ABSTIME, ...)`), not chained relative sleeps -- a late phase does not push out later
  phases/rows. This is deliberate and is the mechanism the panel-flicker investigation's
  per-phase-stall instrumentation (`-t`, gated, see below) measures directly.
- `-r` enables `SCHED_FIFO` at `PANEL_RT_PRIO`/`PWM_RT_PRIO` (80/79) for both threads.
  **Recommended when running `newpanel` and `t30dpy` (or other CPU load) on the same box** --
  see the investigation doc below for the data. Confirmed to cost no additional CPU (12% either
  way, measured); the old `panel_pidp1.c`'s CPU-hog reputation traced to ITS OWN NSAMPLES
  busy-sampling algorithm, not to SCHED_FIFO itself.
- `-t` enables timing diagnostics printed at clean exit: main-loop histogram, pwm-compute-loop
  histogram, and the per-phase/transitioning-stall histograms. These are permanent, zero-cost-
  when-off instruments -- the transitioning-stall one is what actually found and confirmed the
  flicker root cause. Keep them; do not remove as "just debug code."
- A non-privileged CPU-affinity mitigation (`-c`/`-C`, pin panelthread/pwmthread to specific
  cores) was prototyped and tested 02-Jul-26, then REMOVED after live-hardware testing showed
  it made the phase-stall tail WORSE than doing nothing (naive pinning removes the scheduler's
  ability to migrate away from momentary contention; the box-specific core-placement work
  needed to do it properly was never done). Do not re-add this without re-reading the
  investigation doc's account of why it failed -- it is an easy trap to fall into again since
  the reasoning ("no elevated privileges needed") sounds appealing on its own.

## RP1 (Pi 5) hardware context
Pi 5's GPIO is not on-die like Pi 4/earlier -- it lives on a separate RP1 chip reached over a
4-lane PCIe 2.0 link. Per RP1's own datasheet this adds "typically 1us" latency per access,
explicitly flagged as a concern for "bit-bashed protocols" -- exactly what `lightRow()` does.
This is why minimizing GPIO register writes matters more on Pi 5 than it would on Pi 4:
- `src/blincolnlights/panel/gpio/gpiochip_rp1.c`: `rp1_gpio_set_drive()` was rewritten to use
  RP1's atomic SET/CLR registers instead of a read-modify-write on the shared bank OUT
  register (02-Jul-26 fix, keep this).
- `lightRow()` itself only calls `setPin()` on an actual on/off transition now, not on every
  PWM phase for every column (02-Jul-26 change, keep this).
- RP1 also has a real PIO block (1 block, 4 state machines) accessible via PIOLib, but PIOLib
  operations are themselves tunneled over the same PCIe link via a firmware mailbox and carry
  "at least 10 microseconds" latency per operation -- investigated and rejected as a mitigation
  path 02-Jul-26; would add complexity, Pi-5-only platform lock-in, and its own latency tax on
  the same bottleneck link, without removing the actual (CPU-scheduling, not GPIO-latency) root
  cause this investigation found. Don't re-propose PIO without re-reading that reasoning.

## See also
- `Claude/CLAUDE.md` (via the root `CLAUDE.md` indirection) -- project-wide hard constraints,
  C coding standard, sandbox gotchas (including the bash stale-file-read gotcha, which was hit
  twice in this directory's own edit history).
- `src/blincolnlights/pdp1/CLAUDE.md` -- the emulator side of the panel interface (`panel1.c`,
  `pdp1.c`), including the opaque-`Panel*`-in-`pdp1.c` gotcha.
