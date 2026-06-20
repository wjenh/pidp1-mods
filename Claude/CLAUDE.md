# PiDP-1 Emulator -- Project Notes for Claude

## Hard Constraints
- **`install.sh` must NOT be modified** under any circumstances.
- When `pdp1.h` changes, rebuild BOTH the emulator (`make` in `src/blincolnlabs/pdp1/`) AND all IOT plugins (`make` in `IOTs/Type30Display/` and other IOT subdirs). The user's normal workflow only runs `make` in the pdp1 directory; IOT rebuilds must be done explicitly.

## Design Convention -- Shared Struct Field Ordering
New fields added to shared data structures (`PDP1` in `pdp1.h`, `Panel` in `panel_pidp1.h`) go at the **END** of the struct. This preserves ABI compatibility: IOT plugins compiled against an older `pdp1.h` continue to access existing fields at the correct offsets. Inserting fields anywhere else shifts subsequent field offsets and silently breaks all IOT plugins that weren't rebuilt.

## Architecture

### Display (Type 30 / IOT 7)
- Handler: `IOTs/Type30Display/IOT_7.c`
- When `dpy` fires, reads **`AC(pdp1P)` and `IO(pdp1P)` directly from the PDP1 struct** for X/Y coordinates. It does NOT read from `panel->lights3` or `panel->lights4`.
- `panel->lights*` fields are irrelevant to the display IOT. Extra `updatelights()` calls do not corrupt display coordinates.
- The IOT uses a worker thread (`display.c`) and a command buffer; coordinates are stored via `setDisplayData()` and consumed in `iotPoll()`.

### Panel Driver Interface (`panel1.c`)
- `updatelights(pdp, panel)` -- snapshot only. Writes current register state to `panel->lights0`-`lights9`. No side effects on pdp state, no tally.
- `updatelights_pwm(panel, n)` -- tallies `panel->lights*` into `panel->pwmcount[][]` n times and increments `panel->cyclecount` by n. Call once per completed instruction with n = number of machine cycles the instruction occupied.
- `panel->*` lives in shared memory (`/tmp/pdp1_panel`); the panel driver process reads it independently.

### Machine Cycle Structure (`pdp1.c`)
- One call to `cycle()` = one 5 us machine cycle.
- `pdp->inst_cyc` is incremented at the top of every `cycle()` call and reset to 0 on instruction completion or SBS_BREAK start.
- `TP(n)` macro: fires `updatelights()` once per cycle at a random timing pulse (`timernd`), then sets `timernd = TP_unreachable` so no later TP fires in the same cycle.

### Typewriter (tyi / tyo / szf / clf)

**`tyi` is completely asynchronous — it does NOT honour the C (in-out wait) flag or the `i` bit.**
Executing `tyi C` does NOT block waiting for a key. The instruction always executes immediately:
clears IO, copies `pdp->tb` (typewriter buffer) into `IO[bits 0-5]`, clears `tbs` (type-in status
bit). If no key has been pressed, IO ends up 0.

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

The `i` bit (B5 = 010000) and `C` bit (B6 = 004000) have distinct meanings for IOT instructions:

| Modifier | Bit | Behaviour |
|----------|-----|-----------|
| `i`      | B5 (010000) | **Synchronous wait** -- instruction sets ioh=1 and machine spins until device signals ios=1 |
| `C`      | B6 (004000) | **Asynchronous completion** -- instruction fires and returns; device signals ios=1 later; programmer syncs with the `ioh` instruction (730000) |

`tyo`'s base opcode is 730003 (already includes B5). Therefore:
- `tyo i` = 730003 → B5=1, B6=0 → synchronous, blocks until typewriter done.
- `tyo C` = 734003 → B5=1, B6=1 → *intended* async, but still blocks because B5 is in the base.

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

### IOT Instruction Field Layout

From `pdp1.c`: `IR = MB[bits 0-4]` (top 5 bits). `IR_IOT = (IR == 035)`.

- `dev = MB & 077` — low 6 bits of MB (device number, 0-63)
- `ch  = (MB >> 6) & 077` — bits 6-11 of MB (sub-channel / argument)
- B5  = 010000 — i bit (synchronous I/O wait)
- B6  = 004000 — C bit (asynchronous completion)
- `nac = ((MB & (B5|B6)) == B5) || ((MB & (B5|B6)) == B6)` — true if exactly ONE of B5/B6 set

Dynamic IOT plugins (IOT_nn.so) receive `nac` as their `completion` parameter. They signal
completion to the emulator via `IOCOMPLETE(pdp1P)` (sets `pdp1P->ios = 1`), defined in
`IOTs/iotHandler.h`. The machine clears `ioh` at TP8 when `ios=1`.

### The `ioh` Instruction (730000)

A standalone "I/O halt/wait" instruction. B5=1 in 730000 → sets `ioh=1` at TP7. If `ios` is
already 1 (from a prior async completion): TP8 clears `ioh` immediately and execution continues.
If `ios=0`: machine spins until a device calls `IOCOMPLETE()`. Used to synchronise with a
previously issued `tyo C` or similar async IOT.

### Skip instruction direction rule

When writing `skip_instr; jmp target`, think about **when the `jmp` executes** (the no-skip case),
not when the skip fires.

- `sza; jmp bad` → jmp executes when AC ≠ 0 (sza skips over jmp when AC = 0)
- `sza i; jmp bad` → jmp executes when AC = 0 (i bit inverts: sza i skips when AC ≠ 0)

Mental model: choose the skip instruction that describes the **"don't branch"** condition. The jmp
fires whenever that condition is NOT met.

Example — test for error (error flag is non-zero):
```
    and [dserr]     // isolate error bit
    sza             // skip if AC = 0 (no error) -- the "don't branch" condition is AC=0
    jmp halterr     // jmp fires when AC != 0 (error present)
```

Example — loop until connected (dsfcon flag is non-zero when connected):
```
    and [dsfcon]    // isolate connected flag
    sza i           // skip if AC != 0 (connected) -- "don't branch when connected"
    jmp waitcon     // jmp fires when AC = 0 (not yet connected)
```

### sho/shro Instruction Pipeline (IR = 033)
The shift instruction is **pipelined across two `cycle0()` calls**:

| Phase | What happens |
|-------|-------------|
| Fetch cycle TP7-TP10 | Shifts for bits B17-B13 execute (using current MB = sho instruction word, current IR = 033) |
| Fetch cycle TP10 | `CY0_INST_DONE` fires (TRUE for IR >= 030, df1 = 0) -- but AC/IO are only half-shifted |
| Next cycle TP0-TP3 | Shifts for bits B12-B9 execute (old MB/IR still valid before TP4 overwrites them) |
| Next cycle TP3 | `MB = 0`; AC/IO now hold the fully-shifted result |
| Next cycle TP4-TP5 | Next instruction fetched; IR changes |

**`sho_deferred`** (field in `PDP1` struct, placed at end): At TP10 when `IR_SHRO` and `CY0_INST_DONE`, save `inst_cyc` to `sho_deferred` and reset `inst_cyc = 0` instead of tallying. After `MB = 0` at TP3 of the next cycle, if `sho_deferred != 0`: call `updatelights()`, then `updatelights_pwm(panel, sho_deferred)`, clear `sho_deferred`. Also clear `sho_deferred` on SBS_BREAK. This ensures the panel PWM tally sees the fully-shifted AC/IO state, not the intermediate half-shifted state.

### PDP-1D Extended Architecture -- Knowledge from DEC's "The PDP-1D" (27-Dec-2006 writeup)

Source: a DEC historical document (`pdp1d.pdf`, read 19-Jun-2026) describing the PDP-1D, a 1964
DEC menu-of-extensions design (not a fixed/single machine) aimed at time-sharing. Only two were
ever built: serial #45 (BBN) and serial #48 (Stanford) -- **they differ from each other**, and not
every extension below was present on both. Capturing this here because it's the authoritative
source for *why* several `LOG_1D`-tagged instructions already in `pdp1.c` exist, and because it
documents several extensions the emulator does **not** yet implement, in case they're wanted later.

**Already implemented in `pdp1.c`** (confirmed by grep, all logged under `LOG_1D`): `sni`
(644000, skip if IO non-zero), `cmi` (770000, one's-complement IO), `lai`/`lia` (760040/760020,
load AC from IO / load IO from AC -- `lai`+`lia` together = swap AC/IO), `sci`/`scf` (740200/740100,
clear IO / clear program flags), `iif`/`ifi` (744000/742000, OR IO from program flags / OR program
flags from IO), `ida` (740400, increment AC). These match the doc's "Additional Operates and Skips"
and part of the "Protection" opcode-74 group exactly. Present on both serial #45 and #48 per the doc.

**Not yet implemented** -- noted here in case a future task asks for them:

- **Character handling (`LCH`/`DCH`, opcodes 012xxx/014xxx)**: load/store a 6-bit character via an
  indirect "byte pointer" word (bits<0:1> = byte number 1-3, bits<2:17> = word address). Both force
  indirect addressing regardless of the instruction's own indirect bit; that bit instead means
  "automatic" (auto-increment) mode: the byte pointer is incremented *before* use by adding 020000
  (octal), with carry-out bumping the address and resetting byte number to 1. "Ring mode" clamps the
  increment to the low 3 bits, looping over an 8-word/24-character ring buffer (sized for slow
  Teletype-class I/O). `IDC` (741000, opcode-74 group) treats AC itself as a byte pointer in
  automatic mode. Present on both serial #45 and #48.
- **2's-complement arithmetic (serial #45 only, not #48)**: adds a `Link` flag (behaves like a
  program flag, save/restore via the already-implemented `IIF`/`IFI`) alongside the standard
  Overflow flag. New: `TAD` (36xxxx, `Link'AC = AC + M[ea] + Link` -- always adds in Link, so Link
  must be explicitly cleared before starting a multi-precision sequence), `SZL`/`SNL` (740020/750020,
  skip if Link zero/non-zero), `CLL`/`CML`/`STL` (740010/740004/740014, clear/complement/set Link),
  `SCM` (740200, `Link'AC = ~AC + Link` -- one's-complement-and-add-Link; cannot be used as the
  *first* step of a multi-precision 2's-complement sequence unless Link is forced to 1 first via a
  separate `STL`). `IDA` (already implemented above) does NOT set Link and is subject to Ring Mode,
  so per the doc it's "potentially useless" for actual arithmetic use.
- **Protection / "restrict mode" (serial #45 and #48, implemented differently between them)**: no
  base/bounds; instead a violation (IOT/HLT/illegal-opcode while restricted, access to a restricted
  memory bank, or an `LCH`/`DCH` auto-increment crossing a bank boundary while the indirect word is
  `6X7777`) zeroes the instruction register (NOPs the current instruction) and forces an interrupt to
  level 16 (octal). No monitor/user mode distinction -- once restrict mode turns on it stays on until
  any sequence-break level goes active, so the executive necessarily runs as an interrupt handler.
  Granularity/bit count differs: #45 = 16KW granularity, 4 protection bits over the full physical
  space; #48 = 4KW granularity, 8 bits over up to 32KW. #45 additionally had a trap buffer (read to
  diagnose the trap cause; reading clears it) and "memory renaming" (remaps the top 2 bits of the
  program address; doc says it "cannot be bypassed", so likely used to swap non-executive banks to a
  fixed base address for user programs).
- **Clock (both serials)**: fixed 1kHz, 16-bit, caps/wraps at 60,000 (one minute); two separate
  interrupt levels, one firing once/minute, the other every 32ms. Counter is readable; no other
  visible state. (Note: this is architecturally distinct from whatever clock device/IOT the emulator
  already implements for BBN-timesharing-clock support -- worth cross-checking device numbers before
  assuming overlap.)
- **Multi-terminal support, Type 630 multiplexer (both serials)**: scans up to 64 half-duplex
  Teletype lines, one buffer + one ready flag per line. Half-duplex sharing means the single ready
  flag is ambiguous (output-complete vs. input-pending) on simultaneous I/O -- software must track
  whether the last operation on a line was a send or receive to disambiguate. Doc notes PDP-1D's own
  description of the Type 630 is sketchy; a fuller description exists in the PDP-6 Handbook. Type 630
  was reused across early DEC systems through the PDP-7; PDP-8 replaced it with the Type 680.

### I/O Device Timing Model (Reader / Punch / Typewriter) -- Notes for Dynamic-IOT Extraction

Findings from analysing a possible extraction of rpa/rpb/ppa/ppb/tyi/tyo into dynamic IOTs
(similar to `IOTs/Type30Display/IOT_7.c`), cross-checked against DEC's F-17 PDP-1 Maintenance
Manual (1962). **The reader (rpa/rpb) extraction described below has now been implemented** --
see "Reader extraction -- implementation status" near the end of this section. ppa/ppb and
tyi/tyo have NOT been extracted; this section's analysis of them is still design-only, kept here
so it isn't re-derived from scratch next time.

**Reader (rpa/rpb) is a two-phase device, and that split is hardware-faithful.**
`iot_pulse`'s case 001/002 only *arms* the reader: sets `rby`/`rc`/`rcl` (mode/counter/clutch),
`r_time = simtime + RDLY` (a deadline), `rcp` (completion-pulse-requested), clears `rb`. All actual
work -- reading a byte from `r_fd` (the host-side tape-image fd), strobing/shifting into `rb`,
deciding when a full word is assembled, firing `ios`/`rbs`, requesting the sequence break via
`req(pdp, RD_CHAN)` -- happens in `handleio()`, gated by `pdp->simtime` vs `pdp->r_time`.
`readin1()`/`readin2()` re-arm this same mechanism via direct `iot_pulse(pdp, 1, 2, 0)` calls;
the only read-in-specific behavior lives in `handleio()`: `(pdp->rcp || pdp->rim)` auto-transfers
into IO and sets `rim_return` instead of just `rbs`, and the SBS request is skipped while `rim`.
The F-17 manual (9-4b, "Photoelectric Tape Reader Control") describes the identical override on
real hardware: the reader control unit has an explicit "computer is in read-in mode" input that
suppresses the normal completion-pulse-request gating. ppa/ppb (punch) follow the same two-phase
shape via `punon`/`p_time`/`pcp`, just without the byte-shifting/assembly step.

**Critical cadence mismatch for any device-extraction work:** `handleio()` is called
unconditionally every main-loop iteration in `main.c` (right before `pdp->simtime += 5000`),
regardless of run state. By contrast, `dynamicIotProcessorDoPoll()` is only called from inside
`cycle(pdp)` (`pdp1.c` ~2447), and `cycle()` itself is skipped in exactly three situations visible
in `main.c`'s main loop: (1) halted (`!pdp->run`); (2) an HSC DMA steal-cycle
(`processHSCchannels()` returns true -- the loop fakes the steal by just not calling `cycle()`);
(3) read-in's word-assembly iterations, both `if(pdp->rim_cycle) readin1(pdp)` and the
`rim_return`-countdown branch that calls `readin2()` -- neither goes through `cycle()`. The
reader/punch/typewriter's timing is a free-running real-time clock (correct -- real tape
transport and the typewriter don't care whether the CPU happens to be halted), while `DoPoll` was
evidently built for something narrower (display refresh) and was never meant to be a general
device clock.

**The naive fix (duplicate the `DoPoll` call at `handleio()`'s site) is unsafe -- do not do this.**
`dynamicIotProcessorDoPoll()` walks a single shared `pollList`; each entry's `iotPoll()` only
fires once its `curCount` reaches `entryP->pollEnabled` (set by the plugin's own
`enablePolling(n)` call -- the parameter is a cycle *count*, not a boolean, despite the
boolean-looking name/signature in `iotHandler.h`). This mechanism is already in active use by
every existing poll-driven plugin (both displays, the clock, the drum, the line printer, DCS2).
Adding a second call site to the *same* `DoPoll`/`pollList` would silently double (or worse) the
effective call frequency for every one of those plugins during normal run -- exactly the kind of
silent breakage of existing/unknown third-party dynamic IOTs that's off the table. The correct
shape is a **second, independent poll hook reserved for real-time/IO-cadence devices**: a new
optional plugin symbol (e.g. `iotIOPoll`), a new field on `IotEntry`, a new linked list built in
`initializeEntry()` exactly parallel to today's `pollList` construction, and a new
`dynamicIotProcessorDoIOPoll(pdp)` called unconditionally at the same point `handleio()` is
called today. This is purely additive to `dynamicIots.c`/`dynamicIots.h` -- zero existing plugin
(today's or any unknown third-party one) is touched, registered, or affected, because none of
them implement the new symbol. A plugin using this hook gets called every iteration unconditionally
(matching `handleio()`'s own cadence) and does its own `pdp->simtime`-vs-deadline gating inside,
exactly like `handleio()`'s reader/punch/typewriter sections do today -- no cycle-counting throttle
needed, since these devices already self-throttle against simtime.

**Multi-device plugins are an anticipated, supported pattern -- use it for rpa/rpb (and ppa/ppb).**
`dynamicIots.c`'s `initializeEntry()` looks for an `iotHandler` symbol first; if absent, it tries
an `iotAlias()` symbol instead (`int iotAlias(void)`, returns the *real* device number). The
aliased device's `IotEntry.actualEntryP` then points at the real entry, and `dynamicIotProcessor`
transparently redirects through it. This is exactly the shape rpa(001)/rpb(002) need, since they
already share one state machine (`rby`/`rc`/`rcl`/`r_time`/`rb`/`rbs`/`rcp`) in `pdp1.c` today --
implement the real logic once (`IOT_2.so`, say) and ship a one-line `IOT_1.so` that only
implements `iotAlias()` returning 2. ppa/ppb (`punon`/`p_time`/`pcp`) share the identical shape
and should use the same pattern if/when that extraction happens.

**`r_fd`/`p_fd` cannot become plugin-private state.** `main.c` opens `pdp->r_fd`/`pdp->p_fd`
directly at startup (around the `pthread_create(&th, ...)` block) and `handleptr()` in `main.c`
(~line 400) closes and reassigns `pdp->r_fd` at runtime in response to an external command (tape
mount/swap, e.g. from the USB-paper-tape feature or `ptr`/`ptp` tools). A reader/punch plugin must
keep reading these fields live off the `PDP1*` it's passed every call, the same way `handleio()`
does today -- it cannot cache the fd locally, and the fields themselves must stay in the core
struct (consistent with the existing "new fields go at the end, never remove" rule).

**Pre-existing fact, not a new risk:** `dynamicIotProcessor` is already tried before the builtin
switch on `readin1()`/`readin2()`'s direct `iot_pulse` calls. Any user-supplied dynamic IOT
already registered for device 1 or 2 is *already* intercepting read-in's reader pulses today,
with or without any official rpa/rpb extraction. Also pre-existing: the positioning of
`dynamicIotProcessor` ahead of the builtin `switch` was confirmed (by the project owner) to be
intentional, specifically so a dynamic IOT can override a builtin device emulation -- this is the
designed mechanism, not an accident to work around.

**tyi/tyo is higher risk than reader/punch**, for four concrete reasons -- tempered by one
mitigating fact: writing `pf` is not itself unusual for a dynamic IOT (`iotHandler.h` exposes it
directly as the `PFLAGS(pdp1P)` macro, and per the project owner, existing plugins already set
`pf` and even advance `pc`; this is an intended, general-purpose capability of the plugin
architecture, not a special hazard unique to tyi/tyo). The actual risk is more specific: (1) `tyo`
(dev 003) writes PF1 (`pdp->pf |= 040`) as part of a *shared* multi-device state machine rather
than a self-contained one; (2) `tyi` (dev 004) and `tyo` (dev 003) are
different IOT device numbers that share mutable state (`tb`, `tbb`, `tbs`, `typ_time`,
`tyi_wait`, `typ_fd`), so they can't be extracted independently; (3) the output-completion path
branches three ways (plain character / carriage-return / color-shift), each with its own delay,
and the source comments self-describe this as "really much more complicated and overlaps with
the type-in logic"; (4) this exact code already produced a real shipped bug (the `tcp`/`nac`
mixup, fixed 18-Jun-26, see "Typewriter" section above) -- demonstrably subtle enough to get
wrong once already.

**SBS channel numbers used by standard I/O** (`#define`s in `pdp1.c`): `RD_CHAN=1` (reader),
`PUN_CHAN=6` (punch), `TTI_CHAN=7` (typewriter in), `TTO_CHAN=8` (typewriter out). `req(pdp, chan)`
is the standard call any device uses to request a sequence break.

**Host-side I/O is file-descriptor-backed.** `r_fd`/`p_fd`/`typ_fd` are host file/socket/pty
descriptors standing in for the physical reader/punch/typewriter. The emulator's job is reading
or writing bytes through these fds with simulated timing delays layered on top (`RDLY`, `PDLY`,
`TYODLY`), not modeling the electromechanical hardware beyond that timing and framing.

**Reader extraction -- implementation status (19-Jun-2026): done.** The design above (independent
`iotIOPoll` hook, `iotAlias` multi-device pattern) was implemented as designed, plus one gap found
and closed during implementation that's worth recording for the punch/typewriter extractions that
may follow:

- `src/blincolnlights/pdp1/dynamicIots.h`/`.c`: added `IotIOPollP`/`iotIOPoll` (parallel to the
  existing `IotPollP`/`iotPoll`), a new `IotEntry.ioPollP` field, a new standalone `IoPollEntry`
  linked list (`ioPollList`, no cycle-counting -- every registered one fires on every call, unlike
  `pollList`'s `curCount`/`pollEnabled` throttle), and `dynamicIotProcessorDoIOPoll(pdp)` to walk
  it. Confirmed zero effect on the existing `pollList` mechanism -- separate list, separate field,
  separate call site.
- Also refactored `dynamicIotProcessor()`'s lookup/lazy-load logic (handles[dev] + alias-follow +
  lazy `initializeEntry()`) out into a new static `resolveEntry()`, reused by a new exported
  **`dynamicIotOwnsDevice(int dev)`** -- a pure query, no handler call, that core code can use to
  ask "does a dynamic IOT now own this device number?"
- **The gap:** a reader plugin that arms the shared state (`rcl`/`r_time`/`rb`/`rc`/`rby`/`rcp`/
  `rbs`) the same way the old builtin code did would get double-serviced, because `handleio()`'s
  Reader block in `pdp1.c` has no idea a plugin is now responsible for those fields and would keep
  servicing them itself every iteration. Fixed by gating that block on `dynamicIotOwnsDevice(1)`:
  `if(pdp->rcl && pdp->r_time < pdp->simtime && pdp->r_fd >= 0 && !dynamicIotOwnsDevice(1))`. This
  same gate will be needed for Punch (`dynamicIotOwnsDevice(5)`) and Typewriter
  (`dynamicIotOwnsDevice(3)`/`(4)`) if/when those sections get extracted -- they are NOT yet
  gated, since only the reader has been pulled out so far.
- `IOTs/iotHandler.h`: added the `void iotIOPoll(PDP1 *);` forward declaration plugins implement
  against.
- `src/blincolnlights/pdp1/main.c`: added `dynamicIotProcessorDoIOPoll(pdp);` immediately after
  the existing `handleio(pdp);` call, so it shares the exact same gating/cadence handleio() already
  has (whatever that turns out to be) by construction, without needing to re-derive it.
- New plugin `IOTs/Reader/`: `IOT_2.c` is the real handler for rpb (device 2) and, via the device
  number `dynamicIotProcessor` always passes through, also answers for rpa (device 1) when called
  through the alias. `iotHandler()` is a direct port of `iot_pulse`'s old case 001/002 arm logic;
  `iotIOPoll()` is a direct port of `handleio()`'s old Reader block, with `req(pdp, RD_CHAN)`
  replaced by `initiateBreak(RD_CHAN)` since `req()` is private to `pdp1.c`. `IOT_1.c` is a
  one-line `iotAlias()` returning 2, matching the `Type340Display/IOT_16.c` pattern. `Makefile`
  follows the generic `%.o`/`%.so` pattern (no extra `.o` dependencies needed, unlike
  `Type340Display`'s `type340emu.o`). Builds/installs automatically via the existing generic
  `IOTs/install.sh` loop -- no changes needed anywhere in `install.sh` (top-level or `IOTs/`).
- Not yet built/compiled or runtime-tested as of this writing -- next step when authorized.

**Status update (20-Jun-2026): all three of Reader, Punch, and Typewriter have since been
extracted, build-verified, and (Reader/Typewriter) runtime-tested by the project owner on real
hardware/the virtual panel.** `IOTs/Punch/` (`IOT_5.c` ppa-alias, `IOT_6.c` ppb real handler,
`dynamicIotOwnsDevice(5)` guard added) and `IOTs/Typewriter/` (`IOT_3.c` tyo, `IOT_4.c` tyi --
NOT an alias pair, see below -- `dynamicIotOwnsDevice(3)`/`(4)` guards added) both exist now.
Punch uses the cycle-count `iotPoll`/`enablePolling()` mechanism (`PUNCH_POLL_CYCLES`,
`USTOCYCLES(15873)`), matching Clock's idiom, rather than the Reader's unconditional `iotIOPoll`
-- see `IOTs/Punch/IOT_6.c`'s header comment for why. Typewriter needed *both*: tyo (`IOT_3.c`)
uses cycle-count polling (`TYO_POLL_CYCLES`, `USTOCYCLES(100000)`, divides evenly into exactly
20000 cycles), tyi (`IOT_4.c`) uses the unconditional mechanism like the Reader, since its
gating is genuine external fd-readiness, not a fixed delay. tyi/tyo are two **independent** real
plugin entries sharing the same `pdp1P` fields and fd, not an alias pair like rpa/rpb or ppa/ppb
-- they can be in flight simultaneously and need different poll types, so one can't simply
delegate to the other's handler.

**The completion-flag (`B5`/`B6`/`nac`) bug generalized beyond tyo.** `rpa`/`rpb`/`tyo`/`ppa`/
`ppb`/`dpy`/`ioh` (am1's "73-family" mnemonics) all have B5 (010000) baked into their base
opcode. The 18-Jun-26 tyo fix (`pdp->tcp = !!(MB & (B5|B6))` instead of trusting the shared
`nac`) was never ported to Reader/Punch's `rcp`/`pcp`, so `rpa C`/`ppa C` written naively still
hang exactly the way `tyo C` used to (the generic `nac` formula requires *exactly one* of B5/B6
set; both ends up set when `C` is OR'd onto an already-B5 base, and `nac` then comes out 0,
silently). Fixed in `IOT_2.c`/`IOT_6.c` to recompute from raw `MB(pdp1P)&(B5|B6)` the same way.
The safe am1 idiom for genuine non-blocking mode on any of these devices is `<dev>-i C`
(subtract the baked-in `i`, then OR in `C`) -- see `Claude/skill-updates/examples-additions.md`.

**Real bug found and fixed during the Typewriter extraction: case-shift codes were wrong.** The
original `handleio()` Typewriter-output block detected a "shift" character via
`(tb & 076) == 034`, treating 034/035 as case-shift codes. That's wrong: 034/035 are the
Black/Red **ribbon-color** shift codes. The real case-shift codes -- confirmed against this
project's own `bin/decode_fiodec.py`, `bin/encode_fiodec.py`, and `Tools/AM1/parsefns.c`'s
`concise2ascii` table, all three independently agreeing -- are **072 = Lcs** (lower-case shift),
**074 = Ucs** (upper-case shift). Under the old code, genuine case-shift characters fell through
to the "ordinary character" branch (sent to `typ_fd` as garbage) while genuine Black/Red codes
were wrongly swallowed as if they were case shifts.

**Second-pass fix (20-Jun-2026): case and ribbon-color are independent on real Flexowriter
hardware, and the first-pass fix above still conflated them.** The first pass correctly
detected 072/074 but kept the old wire scheme: encode the case bit into bit 6 of every byte sent
to `typ_fd` (`(tbb<<6)|tb`), swallow 034/035 entirely, and let `typtelnet.c`'s `putfio()` read
that bit to decide ANSI red/black color for the GUI typewriter client
(`src/pdp1_periph/typewriter.c`, which just consumes plain ANSI `\e[31m`/`\e[39;49m` escapes and
has no PDP-1-specific knowledge at all). That meant uppercase always rendered red, with no way
to represent genuine ribbon-color codes at all. Real hardware has these as two separate shift
mechanisms. Fixed properly:
- `IOT_3.c` now forwards `tb` to `typ_fd` **completely raw**, no bit-packing, no swallowing, no
  synthetic marker byte. It still updates `tbb` from 072/074 *only* for the front-panel case
  light (`panel1.c`: `if(!pdp->tbb) l9 |= 0200000;`) -- that's the only remaining consumer of
  `tbb`, and it has nothing to do with the wire protocol anymore.
- `typtelnet.c`'s `fio2uni[]` table already had dedicated `Blk`/`Red` sentinel slots in the right
  places (it was clearly designed for this), but both were `#define`d as aliases of the generic
  "ignore" sentinel `XXX` with a comment claiming they "shouldn't be sent, so we ignore." They
  are sent now. Gave them distinct sentinel values and added real handling in `putfio()`,
  symmetric with the existing `Lcs`/`Ucs` handling: color changes only on a genuine `Blk`/`Red`
  table hit, fully decoupled from `ucase` (which still only changes on genuine `Lcs`/`Ucs` hits).
  Dropped the old wire-bit6 `col` logic entirely.
- Knock-on fix: `getfio()`'s local-echo call used to pack `color<<6` onto the echoed byte for the
  old scheme's benefit; simplified to pass the byte through unchanged, since `putfio()` no longer
  looks at that bit. And `telthread()`'s per-connection color reset used to call
  `putfio(0160, telfd)`, relying on the old bit6 logic -- 0160 actually has bit 6 *set*, so this
  never really reset anything even before today's fix. Now writes the reset escape directly.
- `pdp1.c`'s own builtin (now-unreachable-once-the-plugin-loads) copy of this logic was **not**
  touched for either fix -- flagged, not fixed, consistent with the project's "only the loaded
  plugin needs to be correct" scope discipline established with the Reader/Punch extractions.

**Build-system gap found during Typewriter testing: `waitfd`/`closefd` weren't exported for
plugins to dlopen() against.** `IOT_4.c` (tyi) is the first plugin needing the fd-readiness poll
mechanism (`waitfd()`/`closefd()`, defined in `src/blincolnlights/pollfd.c`, declared in
`common.h`) -- Reader/Punch never needed it, they just `read()`/`write()` their own fd directly.
The `pdp1` binary's link line restricts dynamic-symbol exports to an explicit allowlist,
`src/blincolnlights/pdp1/symbol-exports.ldr` (`-Wl,--dynamic-list=symbol-exports.ldr`). Neither
function was on it, so `IOT_4.so` failed at runtime with `undefined symbol: waitfd` -- no
signal, no gdb-visible crash, just the dynamic linker terminating the process; the failure was
only obvious from the actual terminal message, not from a debugger backtrace. Fixed by adding
`"waitfd"` and `"closefd"` to `symbol-exports.ldr`. **Any future plugin needing a `common.h`/
`pollfd.c` function not already on this list will need the same fix** -- worth checking first if
a new plugin's `dlopen()` succeeds but a specific call later fails with "undefined symbol".

## Key Files
| File | Role |
|------|------|
| `src/blincolnlabs/pdp1/pdp1.h` | PDP1 struct definition; shared by emulator and all IOT plugins |
| `src/blincolnlabs/pdp1/pdp1.c` | Core emulator: `cycle()`, `cycle0()`, `defer()`, `cycle1()`, `brkcycle()` |
| `src/blincolnlabs/pdp1/panel1.c` | `updatelights()` / `updatelights_pwm()` -- panel register snapshot and PWM tally |
| `src/blincolnlabs/pdp1/main.c` | Main loop; halt-state and DMA-steal must each call `updatelights()` + `updatelights_pwm(panel, 1)` |
| `src/blincolnlabs/pdp1/display.c` | Type 30 display worker thread; `setDisplayData()` / `getDisplayData()` |
| `IOTs/iotHandler.h` | IOT plugin interface; includes `pdp1.h` -- ABI-sensitive |
| `IOTs/Type30Display/IOT_7.c` | dpy IOT handler |
| `IOTs/Type30Display/Makefile` | Builds `IOT_7.so`; must be run after any `pdp1.h` change |
# SYSTEM PROMPT ENVIRONMENT: ISO C (MODERN GCC) WITH VERBOSE DEFENSIVE STANDARDS

## 1. TARGET ENVIRONMENT & STABILITY PRIORITIES
*   **Language & Toolchain:** ISO C matching modern GCC default behaviors. Procedural/structural hygiene.
*   **Architecture & OS:** Cross-platform Linux (Intel/AMD x86_64 and high-performance ARM, Pi 4 and above).
*   **Optimization Vector:** Prioritize correctness, strict predictability, and structural hygiene over micro-optimizations.

## 2. LIFECYCLE & TRACEABILITY
*   **Iterative Design:** Step-by-step logic expansion with clear validation checkpoints.
*   **Design Mapping:** All code must explicitly map to conceptual modules, state machines, or design requirements.

## 3. EXTREME COMMENTARY MANDATE
*   **No "Self-Documenting" Assumptions:** Comprehensive textual explanations are mandatory.
*   **File/Module Headers:** Must include purpose, architectural scope, dependencies, and execution model.
*   **Function Contracts:** Block comments defining preconditions, postconditions, arguments, return behaviors, and edge-case handling.
*   **Function Comment Style (added 19-Jun-2026):** Every function definition must be preceded by
    a comment block using `//` line comments (not `/* */`), giving a summary of what the function
    does. If the function returns a value, the comment must explicitly describe what the return
    value means (including the meaning of each distinct return value, e.g. 0 vs. non-zero, or what
    a returned pointer refers to and when it can be NULL/0). Void functions need the summary only.
*   **Inline Logic:** Granular commentary detailing the "why" and "how" of branches, state changes, math steps, and pointer manipulation.

## 4. DEFENSIVE SYNTAX & FORMATTING BLUEPRINT
*   Indent with 4 spaces, no tabs in text
*   **Grouping Operators:** Mandatory parenthetical containment `( )` for arithmetic, logical, and bitwise operations to completely isolate precedence bugs.
*   **Control Flow Blocks:** Explicit braces `{ }` for all control constructs (`if`, `while`, `for`, `do`), with braces positioned on separate lines from the body.
*   **Function Calls in Conditionals:** Permitted for common variable assignments where the result can be tested in an obvious, explicit manner (e.g., `if( !(fP = getFile(...)) )`).
*   **Spaces:** No space permitted between a directive/control keyword and its opening parenthesis (e.g., `if(`, `while(`, `switch(`).
*   **Variable Scope Declarations:** 
    *   Globals: Declared strictly at the beginning of the file.
    *   Locals: Declared strictly at the beginning of the function body.
    *   Instantiation: Explicit separation of declaration and initialization in most cases.
*   **Naming Typography:** Camel-hump naming convention.
*   **Pointer Conventions:** C-style pointer declarations (`char *ptrP`) featuring trailing Systems Hungarian Notation suffix matching the pointer type (`P` or `ptr`).

## 5. FILE LAYOUT CONFIGURATION (LOOSE SCOPING ORDER)
1.  Comprehensive File Block Header
2.  Include Files (`#include`)
3.  Preprocessor Defines (`#define`)
4.  Global Variables (Grouped cleanly by data type)
5.  Function Declarations / Definitions
