# PiDP-1 Emulator -- Pending Work

Tracked here (repo-persistent) instead of only in a session's ephemeral task list,
since the in-app task panel does not persist across sessions.

## Open

(nothing currently open)

## Done

### Refactor tyi/tyo handling in pdp1.c
Status: DONE (20-Jun-2026). Extracted to `IOTs/Typewriter/IOT_3.c` (tyo) and `IOT_4.c`
(tyi), build-verified and runtime-tested successfully by the project owner (echo test,
DCS2 round-trip, case-shift). Full detail in `Claude/CLAUDE.md`'s "tyi/tyo" /
"Status update (20-Jun-2026)" sections. Summary of what shipped, for history:

- tyi/tyo are two independent real plugin entries (NOT an alias pair like rpa/rpb or
  ppa/ppb) -- tyo uses cycle-count polling (`TYO_POLL_CYCLES`), tyi uses the
  unconditional `iotIOPoll` mechanism like the Reader, since its gating is genuine
  external fd-readiness rather than a fixed delay.
- Found and fixed a real case-shift bug: the original code detected shift via
  `(tb&076)==034`, which is actually the Black/Red ribbon-color codes, not case
  shift (072 Lcs / 074 Ucs). Confirmed against three independent sources in this
  repo (`bin/decode_fiodec.py`, `bin/encode_fiodec.py`, `Tools/AM1/parsefns.c`).
- Found and fixed a second, deeper bug after the project owner determined (with
  real-hardware research) that case-shift and ribbon-color are genuinely
  independent on real Flexowriter hardware: the first-pass fix above still
  conflated them by encoding case state into the wire protocol and using it to
  drive ANSI color in `typtelnet.c`. Fixed by forwarding `tb` raw from `IOT_3.c`
  and implementing real (previously stubbed-out) Black/Red handling in
  `typtelnet.c`'s `putfio()`, fully decoupled from case tracking.
- Found and fixed a build-system gap: `IOT_4.c` needed `waitfd()`/`closefd()`
  (from `pollfd.c`), which weren't on the `pdp1` binary's dynamic-symbol export
  allowlist (`symbol-exports.ldr`) -- the first plugin to need them, since
  Reader/Punch never did. Symptom was a silent process exit with no signal/gdb
  backtrace, only visible via the actual terminal error message
  ("undefined symbol: waitfd").
- The half-duplex local-echo question (originally flagged 19-Jun-2026, see prior
  revision of this file in version control for the full investigation) was
  resolved by the project owner's own testing: today's software-driven-echo-only
  behavior (no hardware-level forced local echo) is accepted as correct for this
  emulator/teletype configuration.
- `pdp1.c`'s own builtin (now-unreachable-once-plugins-load) copies of the
  case-shift and color-conflation logic were deliberately left unfixed --
  flagged as fix candidates, out of scope since they're fallback-only code.

Generalized fix applied at the same time: the completion-flag (`B5`/`B6`/`nac`)
bug that originally only had a tyo-specific fix (18-Jun-26) was found to affect
every "73-family" device (`rpa`/`rpb`/`ppa`/`ppb` too) and was fixed the same way
in `IOTs/Reader/IOT_2.c` and `IOTs/Punch/IOT_6.c`.
