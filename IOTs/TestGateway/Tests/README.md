# HSC Test Gateway (IOT 44) Test Suite

Tests for the High Speed Channel (HSC) implementation
(`src/blincolnlights/pdp1/highSpeedChannels.c`), exercised through the test-only IOT 44
gateway (`IOTs/TestGateway/IOT_44.c`, device 044). HSC is normally a private, internal
interface used only by device IOTs (the Type 23 Drum and Type 340 Display) -- there is no
real PDP-1 hardware IOT for it. This test-only gateway exists solely because there was
otherwise no way to reach HSC's `HSCallocateChannel`/`HSCfreeChannel`/`HSCexecute`/
`HSCwait`/`HSCgetStatus`/`HSCreset` API from a PDP-1 test program at all. See
`../README.txt` and the header comment in `../IOT_44.c` for the full design rationale.

Written 02-Jul-2026 as part of the project's HSC conformance review, which found (and
fixed) three real bugs in this code and noted that no code anywhere exercised "normal"
(real cycle-stealing) mode at all -- see `src/blincolnlights/pdp1/CLAUDE.md`'s High Speed
Channel section for that history. This suite is the first automated coverage of that mode.

## Building

```
make all          # assemble all test programs
make T01          # assemble T01.am1 only
```

The assembler (`am1`) must be on `$PATH`.

## Running tests

Every test here is self-contained (no companion harness process, unlike some of the DCS2
tests). Load the `.rim` file and start it. The typewriter prints each step as
`<label> pass` or `<label> FAIL`, then a final summary line (`N PASS, N FAIL, N SKIP`).

All five tests use HSC channel 2 (never used by the Drum [channel 1] or Type 340 Display
[channel 3]), and each one frees channel 2 before halting -- including on early-failure
paths -- so the tests can be run in any order, repeatedly, within the same emulator session
without needing a restart between them. See T01.am1's header comment for why this matters:
the HSC channel-assignment table is process-lifetime static state, not cleared between
`.rim` loads (same reason `resetResults` has to be called manually every run -- see
`Am1Includes/TESTUTIL/reportResult.ac`).

---

## T01 -- Alloc/free lifecycle

**What it tests:** `hga` succeeds on a free channel and fails on an already-allocated one
or an out-of-range channel number (0, 6); `hgf` succeeds once and fails on a double free;
`hgs` on a freed channel returns `HSC_ERR` (the gateway's own use-after-free guard); a freed
channel can be reallocated.

**Load:** `T01.rim`

**Expected output:**
```
T01-1 alloc free chan pass
T01-2 double alloc pass
T01-3 chan 0 invalid pass
T01-4 chan 6 invalid pass
T01-5 free allocated pass
T01-6 double free pass
T01-7 status after free pass
T01-8 realloc after free pass
8 PASS, 0 FAIL, 0 SKIP
```

---

## T02 -- IMMEDIATE mode data movement

**What it tests:** `HSC_MODE_TOMEM`/`HSC_MODE_FROMMEM` combined with `HSC_MODE_IMMEDIATE`
in both directions, using `hgl`/`hgd` to stage and verify data through the gateway's
scratch buffers; within-bank address wraparound (a 3-word transfer starting at address
07777 wraps to addresses 07777, 0, 1).

**Load:** `T02.rim`

**Expected output:**
```
T02-1 tomem immediate pass
T02-2 status done pass
T02-3 first word pass
T02-4 last word pass
T02-5 frommem immediate pass
T02-6 readback first pass
T02-7 readback last pass
T02-8 wrap word 0 pass
T02-9 wrap word 1 pass
9 PASS, 0 FAIL, 0 SKIP
```

---

## T03 -- THREADED mode

**What it tests:** `HSC_MODE_THREADED`'s contract -- `hgx` returns `HSC_BUSY` immediately
(unlike IMMEDIATE, which returns `HSC_OK`), `hgw` blocks (a bounded busy-wait) and returns
`HSC_DONE`, and the data itself (moved synchronously inside `HSCexecute()`, same as
IMMEDIATE) is correct.

**Load:** `T03.rim`

**Expected output:**
```
T03-1 hgx returns busy pass
T03-2 hgw returns done pass
T03-3 status after wait pass
T03-4 first word pass
T03-5 second word pass
5 PASS, 0 FAIL, 0 SKIP
```

---

## T04 -- NORMAL (real cycle-stealing) mode

**What it tests:** a transfer with neither `HSC_MODE_IMMEDIATE` nor `HSC_MODE_THREADED`
set -- the real cycle-stealing path (`processChannel()`, driven once per main-loop
iteration from `main.c`, one word per 5us). This mode had no exercise anywhere in the
codebase before this test.

**Load:** `T04.rim`

**Expected output:**
```
T04-1 hgx returns busy pass
T04-2 status done pass
T04-3 first word pass
T04-4 last word pass
T04-5 frommem busy pass
T04-6 readback first pass
T04-7 readback last pass
7 PASS, 0 FAIL, 0 SKIP
```

---

## T05 -- Error handling, reset, and a status-persistence quirk

**What it tests:** `HSCexecute()`'s own parameter validation (count > 4096, bank > 15,
address > 4095, all rejected with `HSC_ERR`); the gateway's use-after-free guard rejecting
`hgx` on an unallocated channel; `hgr` (`HSCreset()`, simulating a front-panel abort)
setting an allocated, idle channel's status to `HSC_ABORT`; and a real, previously
undocumented behavior found while writing this suite -- a channel's status is reset to
`HSC_OK` only the *first* time that channel number is ever allocated in the emulator
process's lifetime, not on every `hga`. A later `hgf`+`hga` cycle leaves status exactly
where the last operation left it (e.g. `HSC_DONE`) until a new transfer changes it. See
`../IOT_44.c` and `src/blincolnlights/pdp1/CLAUDE.md` for more on this.

**Load:** `T05.rim`

**Expected output:**
```
T05-1 bad count pass
T05-2 bad bank pass
T05-3 bad addr pass
T05-4 unallocated chan pass
T05-5 status persists pass
T05-6 hgr sets abort pass
6 PASS, 0 FAIL, 0 SKIP
```

---

## What this suite does NOT (and structurally cannot) cover in am1

**Calling `hgx` on a channel that is already `HSC_BUSY`, and multi-channel priority
arbitration** (`processHSCchannels()` gives each pass's cycle to the highest-priority
channel that wants it on that pass) **cannot be exercised from a single-threaded am1
program.** The reason is structural, not a gap in test-writing effort: a NORMAL-mode
transfer wants *every* main-loop pass until it finishes, and while stealing, the CPU never
executes another instruction. A PDP-1 test program cannot issue a second `hgx` while a
first one is still in flight, because the CPU is frozen for the whole duration; by the time
the *next* instruction runs, the transfer in question is already done. This is faithful to
how real cycle-stealing hardware behaves, but it also means the only way to get two channels
genuinely busy at once is to drive `HSCexecute()` from two independent call sites in C.
`hscharness.c` (below) does exactly that. The same goes for anything that needs control of
`simtime`, a second thread, or a view of the panel calls.

## `hscharness.c` -- standalone C channel test

This covers what the am1 suite above cannot provide itself. It links
`../../../src/blincolnlights/pdp1/highSpeedChannels.c` directly as a plain `.c` file --
no emulator binary, no SDL, no `dlopen`, no IOT plugin mechanism at all -- with stand-ins
for `updatelights()`/`updatelights_pwm()` (which count calls, since the channel code must
make none), `throttleCapFirings`, and the `pdp1P` global that file expects `main.c` to
provide. Its `pass()` does what one main-loop pass does to the channels: a scan, then 5 us
of `simtime`. It checks:

- HSC-1 to 15: two NORMAL-mode channels, the higher one (1) served first even when the
  lower one (5) asked first, and every word correct.
- HSC-16 to 24: a THREADED request's owed cycles paid one per pass, `HSCwait()` finishing it,
  and the channel reusable afterwards.
- HSC-25 to 30: TRUESTEAL moves no word at request time, one word reaches core per steal,
  exactly one steal per word, done on its last tick.
- HSC-31 to 33: per-cycle arbitration -- a NORMAL channel 5 is served in the passes a
  TRUESTEAL channel 1 leaves free, and channel 1 still finishes on time.
- HSC-34 to 36: TRUESTEAL counts `simtime`, so words due across a long instruction are
  stolen right after it, and words due in time a throttle lag-cap firing forgave move
  without a steal.
- HSC-37 to 42: THREADED cycles are stolen even after `HSCwait()` returned; nothing is owed
  while the CPU is stopped; IMMEDIATE steals nothing; `HSCsteal()`.
- HSC-43 to 46: the requester's thread never touches the panel; the scan lights the lamp and
  it goes out after its stretch; the channel code never calls the panel functions.
- HSC-50 and 51: every TRUESTEAL transfer of 1 to 3000 words at the drum's 8.5 us moves exactly
  its count and steals exactly its count. The first version of the rework wrote one word past
  the block for 749 of those sizes.
- HSC-47 to 49: a thread blocked in `HSCwait()` is woken by completion, and by `HSCreset()`.

**Building and running:**
```
make harness
./hscharness
```

Prints one `pass`/`FAIL` line per check and a final failure count; exits nonzero if
anything failed. Some checks print once per word or per pass, so a clean run prints 65
lines. Building with `-DHSC_NO_STEAL` leaves out the `HSCsteal()` checks, for running the
rest against an older `highSpeedChannels.c`; of the checks written for the 23-Sep-2026 rework,
HSC-26, 27, 31, 32, 34-38, 43, 46, 49 and 51 fail against the code before it; the others pass
on both.

## File inventory

| File | Description |
|------|-------------|
| `T01.am1` | Alloc/free lifecycle |
| `T02.am1` | IMMEDIATE mode data movement + wraparound |
| `T03.am1` | THREADED mode |
| `T04.am1` | NORMAL (cycle-stealing) mode |
| `T05.am1` | Error handling, hgr, status-persistence quirk |
| `hscharness.c` | Standalone C channel test: arbitration, TRUESTEAL, owed cycles, lamp, wake-ups |
