# pdp1/ -- Emulator Core Notes for Claude

Scope: `pdp1.c`, `pdp1.h`, `panel1.c`, `main.c`, `display.c`, `highSpeedChannels.c`,
`highSpeedChannels.h` -- the emulator core and its direct panel/display/DMA glue. This file
loads automatically whenever Claude reads a file in this directory; it does not replace
`Claude/CLAUDE.md` (project-wide rules, coding standard, sandbox gotchas) -- read that too,
it is not repeated here.

## Display (Type 30 / IOT 7)
- Handler: `IOTs/Type30Display/IOT_7.c`
- When `dpy` fires, reads **`AC(pdp1P)` and `IO(pdp1P)` directly from the PDP1 struct** for X/Y
  coordinates. It does NOT read from `panel->lights3` or `panel->lights4`.
- `panel->lights*` fields are irrelevant to the display IOT. Extra `updatelights()` calls do
  not corrupt display coordinates.
- The IOT uses a worker thread (`display.c`) and a command buffer; coordinates are stored via
  `setDisplayData()` and consumed in `iotPoll()`.
- Point-plot timing: **35 microseconds total** -- 30us to position the beam (`MOVEDELAY`) plus
  5us to intensify (`DRAWDELAY`), per `IOT_7.c`'s own header comment and #defines. This is the
  authoritative figure (confirmed by the project owner, 11-Jul-26); DEC's F25 I/O Systems Manual
  states 50us for this operation, which is wrong for this device -- see the F25 research summary
  below for the full comparison.

## Lightpen Motion Prediction (`display.c`, added 14-Jul-2026)
Full design writeup and status: `MotionPrediction/lightpen-motion-prediction-2026-07-14.md` at the
repo root. Summary for quick reference:

- A time-normalized velocity/acceleration EMA filter, per screen (`LightpenAxisFilter mfX, mfY`
  in `DisplayControl`), predicts the lightpen's current position from its recent raw samples.
  `checkLightpen()` tests drawn points against the *predicted* position instead of the raw
  last-known sample when enabled, instead of against `ctlP->lpX/lpY` directly.
- Deliberately does **not** change `checkLightpen()`'s signature or add any new coordinate
  readback path -- the predicted position only ever changes *which* point satisfies the existing
  hit test, so both `IOTs/Type340Display/type340emu.c` and `IOTs/Type30Display/IOT_7.c` needed
  zero changes and both benefit automatically. This was a deliberate pivot away from an earlier
  design that would have required `checkLightpen()` to hand back coordinates (fine for Type 340's
  `drc`, but Type 30 has no separate readback at all -- a hit is synchronous with the `dpy`
  instruction that drew the point, so AC/IO already hold the coordinates -- making Type 30
  parity would have meant writing predicted values back into AC/IO, a new kind of side effect
  nothing else here does).
- Floating-point double-precision, ported from the fixed-point `Am1Includes/LIGHTPEN/
  motionFilter.ac` used by `FastPen/newcube.am1`'s `penISR`, but genuinely time-normalized
  (`alpha = 1 - exp(-dt/tau)` from real `currentTime()` deltas) rather than that am1 version's
  implicit fixed-step-per-call assumption -- the emulator core has real wall-clock timestamps the
  PDP-1 program doesn't.
- Ingest happens in `lightpenReader()` (once per coalesced raw client sample); live
  re-extrapolation happens in `checkLightpen()` itself (called far more often, from the emulator
  thread rather than the ~1ms-cadence worker thread), so points tested later between two raw
  samples get a fresher prediction than points tested right after a sample arrives.
- Config via `findConfigurationSetting()` extras (`motionprediction`, `motionvelocityalpha`,
  `motionattackalpha`, `motiondecayalpha`, `motionhorizon`, `motionmaxdelta`, `motionmaxgap`),
  not dedicated `Configuration` struct fields -- explicit project-owner preference, same pattern
  as `aperture`/`pidp1timing`/`t340cachesize`. Off by default. See `pidp1.config.example` for the
  full comments on each key, including why they're named "alpha" but hold seconds, not 0.0-1.0
  coefficients like the audio filters.

## Panel Driver Interface (`panel1.c`)
- `updatelights(pdp, panel)` -- snapshot only. Writes current register state to
  `panel->lights0`-`lights9`. No side effects on pdp state, no tally.
- `updatelights_pwm(panel, n)` -- tallies `panel->lights*` into `panel->pwmcount[][]` n times
  and increments `panel->cyclecount` by n. Call once per completed instruction with n = number
  of machine cycles the instruction occupied.
- `panel->*` lives in shared memory (`/tmp/pdp1_panel`); the panel driver process
  (`src/blincolnlights/panel/driver/newpanel.c`, see its own nested CLAUDE.md) reads it
  independently.

## Machine Cycle Structure (`pdp1.c`)
- One call to `cycle()` = one 5 us machine cycle.
- `pdp->inst_cyc` is incremented at the top of every `cycle()` call and reset to 0 on
  instruction completion or SBS_BREAK start.
- `TP(n)` macro: fires `updatelights()` once per cycle at a random timing pulse (`timernd`),
  then sets `timernd = TP_unreachable` so no later TP fires in the same cycle.

Two things came out worth recording permanently:

1. **A channel's status is not reset to `HSC_OK` on every `HSCallocateChannel()` call** --
   only the first time a given channel number is ever allocated in the process's lifetime
   (`ctlP->isInitialized` latches forever after that). A later free+reallocate cycle on the
   same channel number leaves `status` exactly where the last operation left it (e.g.
   `HSC_DONE`, or `HSC_ABORT` from a prior `HSCreset()`) until a new transfer changes it.
   Not a bug -- real hardware has no concept of a channel's status resetting on
   reassignment either -- but easy to be surprised by if you assume "freshly allocated"
   means "freshly initialized." T05.am1's "status persists" check demonstrates this
   deterministically within a single test run.

2. **Exposing a C API's negative status codes to an 18-bit PDP-1 register needs explicit
   masking, and the result won't match what am1's own negative-literal arithmetic
   produces.** `HSC_ERR` is a plain C `int`, `-1`. The `PDP1` struct's `io`/`ac` fields are
   `Word` (`uint32_t`, standing in for an 18-bit register). A naive `IO(pdp1P) = HSC_ERR;`
   sign-extends into all 32 bits, not 18. Separately, am1 itself emulates the PDP-1's real
   1's-complement arithmetic for constant folding (see `am1-syntax.md`), so am1's own
   literal `[-1]` assembles to `~1` = octal `0777776`, NOT the 2's-complement all-ones
   pattern (`0777777`) a naive C assignment masked to 18 bits would produce. `IOT_44.c`
   solves this with a `setIO()` helper that masks every register write with `WORDMASK`
   before storing, so `HSC_ERR` always shows up as exactly octal `0777777` -- and
   `Am1Includes/HSC/hscgatewaydefs.ah` defines `HSC_ERR` as that literal octal constant,
   not as `[-1]`, so test programs compare against a value that's actually guaranteed to
   match. Worth remembering for any future IOT that exposes a C status enum containing
   negative values to PDP-1 code -- neither the Drum nor Type 340 IOTs had ever needed to
   before this.

**Structural limitation, not fixed by the am1 suite:** true multi-channel priority
arbitration (`processHSCchannels()`'s "scan low to high, first busy channel wins, others
wait entirely" logic) cannot be exercised from a single-threaded am1 program at all --
starting one HSC transfer freezes the emulated CPU (via the main loop's steal-every-tick
behavior) until it finishes, so a PDP-1 program can never get a second channel busy while
the first is still in flight to actually observe the scan choosing between them. See
`IOTs/TestGateway/Tests/README.md`'s "What this suite does NOT (and structurally cannot)
cover in am1" section for the full explanation. This gap is filled instead by
`IOTs/TestGateway/Tests/hscharness.c`, a standalone C program that links
`highSpeedChannels.c` directly (no emulator binary, SDL, or `dlopen` involved) and drives
two channels concurrently from plain C, bypassing the single-CPU-thread constraint
entirely. All 15 of its checks (27 individual pass/FAIL lines, some inside per-word loops)
pass against the current code, confirming the priority scan correctly lets a
higher-priority channel (1) fully drain before a lower-priority one (5) is touched at all,
even when the lower-priority request was issued first.

## Typewriter (tyi / tyo / szf / clf)

**`tyi` is completely asynchronous -- it does NOT honor the C (in-out wait) flag or the `i`
bit.** Executing `tyi C` does NOT block waiting for a key. The instruction always executes
immediately: clears IO, copies `pdp->tb` (typewriter buffer) into `IO[bits 0-5]`, clears `tbs`
(type-in status bit). If no key has been pressed, IO ends up 0.

**`tyi` does NOT clear Program Flag 1 (PF1).** PF1 (`pdp->pf` bit 040, i.e. `decflg(1) = 040`)
is set by the emulator when a key is struck (`pdp->pf |= 040`). It is cleared **only** by the
`clf 1` instruction (`pdp->pf &= ~decflg(MB)`). Without an explicit `clf 1` after `tyi`, the
next poll loop finds PF1 still set and reads the stale buffer instead of waiting for a new key.

**Correct keyboard input idiom:**
```
    local getkey
getkey,
    szf 1 i     // skip if PF1 IS set (i bit inverts: szf 1 = skip if PF1=0, szf 1 i = skip if PF1!=0)
    jmp getkey  // PF1 clear -- no key yet, keep polling
    tyi         // PF1 set: IO <- typewriter buffer [bits 0-5]; clears tbs
    clf 1       // clear PF1 -- REQUIRED; tyi does not clear it
    endlocal
```

**`tyo i` and `tyo C` -- synchronous vs asynchronous output:**

The `i` bit (B5 = 010000) and `C` bit (B6 = 004000) have distinct meanings for IOT
instructions:

| Modifier | Bit | Behaviour |
|----------|-----|-----------|
| `i`      | B5 (010000) | **Synchronous wait** -- instruction sets ioh=1 and machine spins until device signals ios=1 |
| `C`      | B6 (004000) | **Asynchronous completion** -- instruction fires and returns; device signals ios=1 later; programmer syncs with the `ioh` instruction (730000) |

`tyo`'s base opcode is 730003 (already includes B5). Therefore:
- `tyo i` = 730003 -> B5=1, B6=0 -> synchronous, blocks until typewriter done.
- `tyo C` = 734003 -> B5=1, B6=1 -> async; fires typewriter and returns immediately; use `ioh`
  (730000) to sync when result is needed.

**`tyo i` is the safe choice** for simple programs that need to wait for output. `tyo C` can be
used with the `ioh` (730000) instruction to overlap computation with typewriter output.

**Note:** There was a pdp1.c bug (`pdp->tcp = nac`; nac=0 when both B5 and B6 set) that caused
`tyo C` to hang forever. Fixed 18-Jun-26: `pdp->tcp = !!(MB & (B5 | B6))`.

**szf / clf / stf quick reference (program flag instructions):**

| Instruction | Encoding | What it does |
|-------------|----------|-------------|
| `szf 1`     | 650001   | Skip if PF1 is NOT set (zero) |
| `szf 1 i`   | 650001 with i-bit | Skip if PF1 IS set (non-zero) -- i bit inverts skip sense |
| `clf 1`     | 760001   | Clear PF1 |
| `stf 1`     | 760011   | Set PF1 |

Note: `szf`/`stf`/`clf` are OPR-class instructions, NOT IOT. `clf` and `stf` are in the 76xxxx
range; `szf` assembles to the 65xxxx range.

## sho/shro Instruction Pipeline (IR = 033)
The shift instruction is **pipelined across two `cycle0()` calls**:

| Phase | What happens |
|-------|-------------|
| Fetch cycle TP7-TP10 | Shifts for bits B17-B13 execute (using current MB = sho instruction word, current IR = 033) |
| Fetch cycle TP10 | `CY0_INST_DONE` fires (TRUE for IR >= 030, df1 = 0) -- but AC/IO are only half-shifted |
| Next cycle TP0-TP3 | Shifts for bits B12-B9 execute (old MB/IR still valid before TP4 overwrites them) |
| Next cycle TP3 | `MB = 0`; AC/IO now hold the fully-shifted result |
| Next cycle TP4-TP5 | Next instruction fetched; IR changes |

**`sho_deferred`** (field in `PDP1` struct, placed at end): At TP10 when `IR_SHRO` and
`CY0_INST_DONE`, save `inst_cyc` to `sho_deferred` and reset `inst_cyc = 0` instead of
tallying. After `MB = 0` at TP3 of the next cycle, if `sho_deferred != 0`: call
`updatelights()`, then `updatelights_pwm(panel, sho_deferred)`, clear `sho_deferred`. Also
clear `sho_deferred` on SBS_BREAK. This ensures the panel PWM tally sees the fully-shifted
AC/IO state, not the intermediate half-shifted state.

## PDP-1D Extended Architecture -- Knowledge from DEC's "The PDP-1D" (27-Dec-2006 writeup)

Source: a DEC historical document (`pdp1d.pdf`, read 19-Jun-2026) describing the PDP-1D, a
1964 DEC menu-of-extensions design (not a fixed/single machine) aimed at time-sharing. Only two
were ever built: serial #45 (BBN) and serial #48 (Stanford) -- **they differ from each other**,
and not every extension below was present on both. Capturing this here because it's the
authoritative source for *why* several `LOG_1D`-tagged instructions already in `pdp1.c` exist,
and because it documents several extensions the emulator does **not** yet implement, in case
they're wanted later.

**Already implemented in `pdp1.c`** (confirmed by grep, all logged under `LOG_1D`): `sni`
(644000, skip if IO non-zero), `cmi` (770000, one's-complement IO), `lai`/`lia`
(760040/760020, load AC from IO / load IO from AC -- `lai`+`lia` together = swap AC/IO),
`sci`/`scf` (740200/740100, clear IO / clear program flags), `iif`/`ifi` (744000/742000, OR IO
from program flags / OR program flags from IO), `ida` (740400, increment AC). These match the
doc's "Additional Operates and Skips" and part of the "Protection" opcode-74 group exactly.
Present on both serial #45 and #48 per the doc.

**Not yet implemented** -- noted here in case a future task asks for them:

- **Character handling (`LCH`/`DCH`, opcodes 012xxx/014xxx)**: load/store a 6-bit character via
  an indirect "byte pointer" word (bits<0:1> = byte number 1-3, bits<2:17> = word address).
  Both force indirect addressing regardless of the instruction's own indirect bit; that bit
  instead means "automatic" (auto-increment) mode: the byte pointer is incremented *before* use
  by adding 020000 (octal), with carry-out bumping the address and resetting byte number to 1.
  "Ring mode" clamps the increment to the low 3 bits, looping over an 8-word/24-character ring
  buffer (sized for slow Teletype-class I/O). `IDC` (741000, opcode-74 group) treats AC itself
  as a byte pointer in automatic mode. Present on both serial #45 and #48.
- **2's-complement arithmetic (serial #45 only, not #48)**: adds a `Link` flag (behaves like a
  program flag, save/restore via the already-implemented `IIF`/`IFI`) alongside the standard
  Overflow flag. New: `TAD` (36xxxx, `Link'AC = AC + M[ea] + Link` -- always adds in Link, so
  Link must be explicitly cleared before starting a multi-precision sequence), `SZL`/`SNL`
  (740020/750020, skip if Link zero/non-zero), `CLL`/`CML`/`STL` (740010/740004/740014,
  clear/complement/set Link), `SCM` (740200, `Link'AC = ~AC + Link` -- one's-complement-and-
  add-Link; cannot be used as the *first* step of a multi-precision 2's-complement sequence
  unless Link is forced to 1 first via a separate `STL`). `IDA` (already implemented above)
  does NOT set Link and is subject to Ring Mode, so per the doc it's "potentially useless" for
  actual arithmetic use.
- **Protection / "restrict mode" (serial #45 and #48, implemented differently between them)**:
  no base/bounds; instead a violation (IOT/HLT/illegal-opcode while restricted, access to a
  restricted memory bank, or an `LCH`/`DCH` auto-increment crossing a bank boundary while the
  indirect word is `6X7777`) zeroes the instruction register (NOPs the current instruction) and
  forces an interrupt to level 16 (octal). No monitor/user mode distinction -- once restrict
  mode turns on it stays on until any sequence-break level goes active, so the executive
  necessarily runs as an interrupt handler. Granularity/bit count differs: #45 = 16KW
  granularity, 4 protection bits over the full physical space; #48 = 4KW granularity, 8 bits
  over up to 32KW. #45 additionally had a trap buffer (read to diagnose the trap cause; reading
  clears it) and "memory renaming" (remaps the top 2 bits of the program address; doc says it
  "cannot be bypassed", so likely used to swap non-executive banks to a fixed base address for
  user programs).
- **Clock (both serials)**: fixed 1kHz, 16-bit, caps/wraps at 60,000 (one minute); two separate
  interrupt levels, one firing once/minute, the other every 32ms. Counter is readable; no other
  visible state. (Note: the emulator already implements this for BBN-timesharing-clock support --
  worth cross-checking device numbers before assuming overlap.)
- **Multi-terminal support, Type 630 multiplexer (both serials)**: scans up to 64 half-duplex
  Teletype lines, one buffer + one ready flag per line. Half-duplex sharing means the single
  ready flag is ambiguous (output-complete vs. input-pending) on simultaneous I/O -- software
  must track whether the last operation on a line was a send or receive to disambiguate. Doc
  notes PDP-1D's own description of the Type 630 is sketchy; a fuller description exists in the
  PDP-6 Handbook. Type 630 was reused across early DEC systems through the PDP-7; PDP-8
  replaced it with the Type 680.

## BBN PDP-1 Timesharing System -- Research Summary (21-Jun-2026)

Long-term, not-yet-committed goal; BBN software appears lost. Key facts established by
research (primary source: Yates 1962 MIT thesis ESL-R-140, read in full; describes a 2-console
MIT installation "based on this design," not BBN's own machine, but same architectural
lineage):

- Drum swap (full 4096-word save+restore): **33ms**. Emulator's own `Docs/UsingType23Drum.md`
  says ~35ms -- close enough to be a good approximation, but Fredkin built *custom* drum
  control hardware at BBN (not a stock DEC interface), so an exact match is not guaranteed.
- Scheduler tick: **~8.3ms** (one quarter drum revolution), tapped from the drum rotation
  counter -- NOT from a dedicated clock. The 1kHz RCK clock in the PDP-1D supplement is a 1964
  addition, two years after the thesis. Do not assume either timing source maps to the existing
  BBN-timesharing-clock IOT without checking which one it actually models.
- Per-user quantum: ~400ms+ (swap time is ~8% of quantum per the thesis).
- A remembered "fixed 33ms timeout" conflates two real constants: the 33ms swap duration and
  the 8.3ms tick period. The timeout IS the tick (~8.3ms), not the swap.
- BBN's FASTRAND drum was for program/file storage, unrelated to the timesharing swap drum.

## F25 DEC "PDP-1 Input-Output Systems Manual" -- Research Summary (11-Jul-2026)

Source: `F25_PDP1_IO.pdf` in the repo root (DEC's preliminary I/O Systems Manual for PDP-1).
Scanned document with no text layer; read via OCR. Covers the standard iot instruction
format, the basic and Type 20 (16-channel) Sequence Break System, High Speed Channels, and a
full appendix of device commands.

**Scope note:** most of the appendix's device list -- photoelectric card reader/punch (Type
40/41-523), basic magnetic tape (Type 51), high-speed magnetic tape (Type 52), the standalone
clock reset/read `rsk`/`rdk`, timer `stm`, relay buffer `srb`, analog-to-digital converter
`cnv`/`rcb`, core memory expansion (Type 11/14) `cfd`/`cdf` -- is **not implemented** in this
project (confirmed against `IOTs/KnownIOTs.txt`, which enumerates exactly what is implemented)
and is out of scope. Recorded here so a future read of the same PDF doesn't re-raise these as
missing features. What follows is only the knowledge relevant to devices this project actually
implements.

**Table I -- historical confirmation of the `nac`/completion-pulse formula.** F25's Table I for
the standard iot sync bits 5/6 states the completion-pulse-enable signal is the exclusive-or of
bits 5 and 6 -- exactly the formula already implemented as `nac` (`nac = exactly one of B5/B6
set`; see the IOT Instruction Field Layout section in the root `Claude/CLAUDE.md`). The four
combinations per Table I:

| B5 | B6 | Behavior |
|----|----|----------|
| 0 | 0 | Continue, no wait; completion pulse disabled |
| 0 | 1 | Continue, no wait; completion pulse enabled (this project's async `C` idiom) |
| 1 | 0 | Wait then continue; completion pulse enabled (this project's sync `i` idiom) |
| 1 | 1 | Wait then continue; completion pulse DISABLED |

The 1,1 row is a hang hazard on real 1962 hardware too (waiting for a pulse that is switched
off), unless a device's restart line bypassed the enable/disable flip-flop and pulsed restart
directly -- F25 notes some devices did this. This is the exact failure mode behind the
already-fixed `tyo C` hang (pdp1.c, 18-Jun-26, described above) and the DCS2 `rwe i` hang (see
the pdp1-emulator skill's devices.md, "IOT Completion Call Semantics" section) -- both bugs are
now confirmed to match the original hardware's own documented behavior for this bit
combination, not an emulator-specific quirk.

**Type 30 display timing -- F25's stated figure is wrong; already corrected above.** F25 states
the point-plotting oscilloscope (this project's Type 30 display, `dpy`/iot 7) takes 50
microseconds per point. The project's actual, already-implemented figure (see the Display
section above) is 35us (30us move + 5us intensify), confirmed authoritative by the project
owner 11-Jul-26. F25's 50us does not apply to this device.

**IOT device-number overlaps are not inherently suspicious.** F25's own appendix lists two
devices -- analog-to-digital converter `cnv` and card reader `rac` -- at the same code (iot
XX41). This is expected for the original hardware: such devices were mutually-exclusive add-on
options, and a given physical PDP-1 would only ever have one installed. Separately, and
confirmed by the project owner (11-Jul-26): this project's own current implementation also has
at least one known, intentional overlap of its own -- the `dpy` reorigin-coordinates option
conflicts with the lightpen `sdb` iot; when both are enabled, `sdb` takes priority (see
`README.md`). Neither case is a bug. Don't treat a bare code/feature overlap -- in F25 or in
this project -- as evidence of an error without other confirmation.

**Type 20 (16-channel) Sequence Break System -- opcodes cross-checked.** F25's appendix gives
`esm`=iot55, `lsm`=iot54, `isb`=iot52, `asc`=iot51, `dsc`=iot50 (channel number NN embedded in
instruction bits 8-11). These match this project's already-documented opcodes exactly
(0720055/054/052/051/050 -- see `Claude/skill-updates/sbs-architecture.md`), confirming the
SBS16 instruction set faithfully follows the original Type 20 hardware. One implementation
difference: this project takes the target channel from IO (per `sbs-architecture.md`'s "channel
specified in IO"), rather than encoding it in instruction bits 8-11 as F25's original hardware
did -- a deliberate difference, not a bug, but worth knowing if ever cross-referencing an old
PDP-1 program listing that used the NN-embedded form directly.

## See also
- `Claude/CLAUDE.md` (via the root `CLAUDE.md` indirection) -- project-wide hard constraints,
  C coding standard, IOT plugin ABI/completion convention, sandbox gotchas. Always read that
  too; it is not duplicated here.
- `src/blincolnlights/panel/driver/CLAUDE.md` -- the panel driver PROCESS side (newpanel.c),
  including the panel-flicker root-cause investigation summary.
- `IOTs/TestGateway/CLAUDE.md` -- the HSC test suite (IOT 44 + am1 tests + `hscharness.c`)
  referenced throughout the High Speed Channel section above.
- `IOTs/Type340Display/CLAUDE.md` -- the other HSC consumer
- `MotionPrediction/lightpen-motion-prediction-2026-07-14.md` -- full design writeup for the
  lightpen motion prediction feature summarized in the "Lightpen Motion Prediction" section above
