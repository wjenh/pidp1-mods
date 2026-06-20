# Additions for am1-pdp1-assembler/references/examples.md
#
# Append these sections to the end of examples.md.
#
# Target file location (Windows):
#   %AppData%\Claude\local-agent-mode-sessions\skills-plugin\
#   ...\skills\am1-pdp1-assembler\references\examples.md
#
# ─────────────────────────────────────────────────────────────────────────────

## Skip instruction direction rule

When writing `skip_instr; jmp target`, think about **when the `jmp` executes** (the no-skip case),
not when the skip fires.

- `sza; jmp bad` → jmp executes when AC ≠ 0
- `sza i; jmp bad` → jmp executes when AC = 0 (i bit inverts the skip condition)

**Mental model:** choose the skip instruction that describes the *"don't branch"* condition.
The jmp fires whenever that condition is not met.

Error check (branch on non-zero error flag):
```
    and [dserr]     // isolate error bit
    sza             // skip if AC = 0  (no error — the "don't branch" condition)
    jmp halterr     // jmp fires when AC != 0 (error present)
```

Loop until connected (branch back when flag is zero):
```
    and [dsfcon]    // isolate connected flag
    sza i           // skip if AC != 0  (connected — "don't branch when connected")
    jmp waitcon     // jmp fires when AC = 0 (not yet connected)
```

## Keyboard input (tyi) idiom

`tyi` is completely asynchronous — it does NOT honour the C (in-out wait) flag. The correct
pattern polls Program Flag 1 (PF1), reads with bare `tyi`, then clears PF1 with `clf 1`.
PF1 is NOT cleared by `tyi`; omitting `clf 1` causes the next poll to return immediately with
the stale buffer value instead of waiting for a new keypress.

```
Keyboard echo loop example
#include <whatever_IOTs_you_need.ah>

100/
loop,
    local getkey
getkey,
    szf 1 i         // skip if PF1 is set (key in typewriter buffer)
    jmp getkey      // PF1 clear -- no key yet, keep polling
    tyi             // PF1 set: IO <- typewriter buffer [bits 0-5]; clears tbs
    clf 1           // clear PF1 -- REQUIRED; tyi does NOT clear PF1
    endlocal

    tyo C           // print it; tyo C blocks until done (C flag IS honoured for tyo)
    jmp loop

start loop
```

**Rule summary:**
- `tyi C` → wrong; C is ignored, reads immediately (possibly garbage)
- `szf 1 i; jmp poll; tyi; clf 1` → correct idiom
- `tyo i` → synchronous; blocks until character printed (safe, simple)
- `tyo C` → asynchronous; fires typewriter and returns; requires `ioh` (730000) to sync later
  (requires pdp1.c fix applied 18-Jun-26; `tyo C` hung permanently before that fix)

## Asynchronous typewriter output with `tyo C` and `ioh`

Use `tyo C` when you want to overlap computation with typewriter output. The `ioh` instruction
(730000) acts as a barrier: it spins until `ios=1` (device completion) or continues immediately
if the typewriter already finished.

```
    tyo C           // fire typewriter asynchronously; returns immediately

    // ... do other work here while character is printing ...

    ioh             // 730000: wait here until typewriter signals completion
                    //   (or continue instantly if it already finished)

    // typewriter is now idle; safe to issue next tyo
```

`ioh` = 730000: B5=1, device=0. The device-0 case in iot_pulse is a no-op; the B5 flag alone
provides the wait-for-ios mechanism. This is the standard PDP-1 "I/O halt" synchronisation
primitive, usable with any asynchronous IOT.

## Generalizing the `tyo C` fix: ALL "73-family" devices need the `-i C` idiom, not just tyo

The note above ("tyo C ... requires pdp1.c fix applied 18-Jun-26") is true but easy to
over-generalize. Re-checked 19-Jun-2026 while writing a punch (ppa/ppb) test program:

**Root cause.** `rpa`, `rpb`, `tyo`, `ppa`, `ppb`, `dpy`, `ioh` (the "73-family" reserved
mnemonics, see permsyms.def) all have the wait bit (B5, value 010000) baked into their base
opcode already. Writing the bare mnemonic, or OR-ing in `i` explicitly, changes nothing.
Writing `C` (B6, 004000) on top of one of these bases sets *both* B5 and B6, and the
runtime's generic nac formula in `iot()`:
```
nac = ((MB & (B5|B6)) == B5) || ((MB & (B5|B6)) == B6);
```
only recognizes *exactly one* bit set — both-set matches neither arm, so `nac` (and anything
fed from it) comes out 0. No completion is requested, silently.

**The 18-Jun-26 fix only covers tyo.** It changed `pdp->tcp`'s assignment from `pdp->tcp =
nac` to `pdp->tcp = !!(MB & (B5|B6))` — i.e. tyo's own handler stopped trusting the shared
`nac` value and started asking "is either bit set" instead of "is exactly one bit set". That
fix is local to tyo's case in iot_pulse(); it does **not** change the generic `nac` formula
itself, and it was never applied to ppa/ppb (IOT_6.c, the dynamic Punch plugin added
19-Jun-2026) or to rpa/rpb (IOT_2.c, the dynamic Reader plugin). For every device *except*
tyo, writing `<dev> C` (or `<dev> i C`) naively still has the original hang/silent-drop bug:
`nac`/`completion` comes out 0, so the plugin never sets `ios`, and a subsequent `ioh` (or
any code waiting on the device) blocks forever.

**The safe, general idiom — works for every device in this family, not just tyo:**
```
ppa-i C     // = 0724005: B5 cleared, B6 set only -- genuine non-blocking punch
...
ioh         // bare ioh (= 730000) already has B5 baked in -- blocks until ios
```
Subtracting `i` first clears the baked-in B5 cleanly (no borrow/carry issues across the
other bits, confirmed numerically, not just by inspection), leaving only B6 set, which
matches the `== B6` arm of the nac formula exactly. This is the same idiom already used
for `dpy` (`dpyc` is literally defined as `dpy-i C`) and demonstrated working in
`pong.am1`/`lightpen.am1` (`dpy-i C ...`/`gpl C`, each followed later by bare `ioh`).

**Rule of thumb:** for any "73-family" device, don't trust `<dev> C` to work just because
it happens to for tyo. Always use `<dev>-i C` when you want the non-blocking/complete-flop
behavior, and verify with `<expr> & 014000` -- it should be `010000` (wait), `004000`
(complete), or `0` (neither), but never `014000` (both -- broken, nothing will signal).

## Gotcha: shift/operator counts are parsed in the CURRENT radix too, not just operands

Found 19-Jun-2026, confirmed by running the resulting tape on real hardware: under an
`octal` directive (the default), **every bare number in the source is octal, including a
shift count on the right-hand side of `<<`/`>>`** -- it's not special-cased to decimal just
because it "looks like" a small bit-count.

```
octal
... << 12     // BUG: 12 here is octal 12 = decimal 10. Shifts by 10 bits, not 12.
... << 0d12   // correct: 0d forces decimal regardless of the surrounding radix.
```

This bit someone writing `(word & 0000077) << 12` intending "shift the low 6 bits up into
bits 12-17 of an 18-bit word" -- decimal 12 is the right shift count, but spelled `12` under
`octal` it silently means 10, producing a value shifted 2 bits short with no error or
warning. The failure is silent: the assembler accepts it, and the bug only shows up as
wrong data at runtime (in this case, a corrupted byte on a punched tape).

**Rule of thumb:** any bare numeric literal is subject to the current `octal`/`decimal`
directive, *including* shift counts, loop/table counts, and anything else that's "obviously"
meant to be a small decimal bit/byte count -- not just the main data values. When a literal's
intended value depends on bit-width (8, 12, 16, 18, 24, 32...) rather than being a natural
quantity in whatever radix is already in effect, prefer the explicit `0d`/`0o`/`0x` prefix
over relying on context, even when the current value happens to be unambiguous (e.g. `6` is
the same in octal and decimal, but `12`, `18`, etc. are not). This is the same advice as
"best practice is to always close the constant" elsewhere in the syntax doc -- be explicit
rather than relying on the literal matching the ambient radix by coincidence.
